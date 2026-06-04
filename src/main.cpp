/*****************************************************************************/
// Sneaky Gestures V2: MIDI BLE Gestural Glove
// https://github.com/sneak-thief/Sneaky-Gestures-V2
//
// Target: Seeed XIAO nRF52840 Sense  |  Framework: Adafruit nRF52 BSP
// ---------------------------------------------------------------------------
//
// A wearable MIDI controller transmitting over BLE. The thumb makes contact
// with conductive pads on the fingers, read via a 74HC4067 16-channel mux.
//
// INPUTS 0-15
//   Channels 3-14 : 12 finger-pad note buttons (pinky base=low, index tip=high)
//   Channel 0     : Octave up   (release) / hold 1s = tap tempo mode
//   Channel 1     : Octave down (release) / hold 1s = note quantize cycle
//   Channel 2     : Index modal (hold = flex sensor notes + AccelY CC)
//   Channel 15    : Pinky palm  (tap = spread cycle / hold 1s = scale cycle)
//   A1            : Thumb FSR   → aftertouch (AFTERTOUCH_DELAY_MS = 30ms after NoteOn)
//   A2            : Index flex  → quantized notes + LED bar while ch2 held
//   IMU           : AccelX→CC11, AccelY→CC71, double-tap→battery display
//
// SCALE QUANTIZATION
//   20 scales (0=chromatic). Each button maps to a unique scale degree;
//   overflow buttons play the next octave (no repeated notes).
//   rootTranspose (±5): rotates scale root pitch class (ch9↑/ch12↓ in tap tempo)
//   keyTranspose  (±5): raw semitone shift post-quantization (ch3↑/ch6↓)
//
// TEMPO & QUANTIZATION
//   Internal tempo (default 120 BPM) set via tap tempo (8 octave-up presses).
//   External MIDI clock (24 PPQN) overrides internal tempo via EMA.
//   Note quantization: OFF / 1/16 / 1/32 (cycled by octave-down 1s hold).
//
// LED STRIP  7x SK6812 RGBW along knuckles (0=index, 6=pinky)
//   Priority: orange flash > tap tempo > battery > modal indicators >
//             flex bar > finger note animations
//   Note animations: source LED red→note-color on press, ripple outward,
//                    held at note color, fade note-color→red→black on release.
//   Per-finger colors: base=blue, middle=cyan, tip=purple.
//   Modal displays (700ms each): octave, spread, scale, quantize mode.
/*****************************************************************************/

#include "Wire.h"
#include "Adafruit_TinyUSB.h" // USB Library
#include <light_CD74HC4067.h> // 74HC4067 Library
#include <bluefruit.h>        // Adafruit Bluefruit BLE Library
#include <MIDI.h>             // MIDI Library
#include "LSM6DS3.h"          // IMU Library
#include <math.h> // expf, fabsf for LED ripple math
#include <string.h>  // memcpy, strncmp for the serial patch console
#include <stdlib.h>  // atoi for the serial patch console
#include <Adafruit_LittleFS.h>      // LittleFS over internal flash
#include <InternalFileSystem.h>     // nRF52 internal flash filesystem (Adafruit BSP)
#include "TempoPitchShifter.h"      // tempo->pitch-bend retune helper
#include "LedDisplay.h"             // LED strip rendering / animation module
#include "GloveState.h"             // shared musical/tempo/preset globals (extern)
#include "ScaleQuant.h"             // scales, pitch mapping, key spread, quantize grids
#include "TempoControl.h"           // setTempo()
#include "Presets.h"                // patch persistence + serial console
using namespace Adafruit_LittleFS_Namespace;

#include "DebugSerial.h"   // DBG_* / BOOT_* logging macros (shared)


// - Neopixel Defines -
#define NEOPIXEL_ENABLED 1            //  Enable/disable Neopixel LED lights

#ifdef NEOPIXEL_ENABLED
#include <Adafruit_NeoPixel.h> // Neopixel Library

// Which pin on the Arduino is connected to the NeoPixels?
#define LED_PIN 5

// How many NeoPixels are attached to the Arduino?
#define LED_COUNT 7

// NeoPixel brightness. Now a runtime parameter (savable per preset) cycled by
// channel 7 in the preset browser through 7 levels. BRIGHTNESS is the default
// applied at boot before any preset is loaded.
#define BRIGHTNESS 30
// 7 selectable brightness levels: off, 10, 30, 60, 100, 180, 255.
extern const uint8_t BRIGHTNESS_LEVELS[7] = {0, 10, 30, 60, 100, 180, 255};
extern const int BRIGHTNESS_LEVEL_COUNT = 7;
int brightnessLevel = 2; // index into BRIGHTNESS_LEVELS (2 = 30, matches BRIGHTNESS)
// Set true when something changes the global strip brightness (which does NOT
// alter the per-pixel curPix values), forcing the next frame to re-push so the
// new brightness is actually displayed.
volatile bool ledForceRepush = false;

// Declare our NeoPixel strip object:
Adafruit_NeoPixel strip(LED_COUNT, LED_PIN, NEO_GRBW + NEO_KHZ800);

#endif // NEOPIXEL_ENABLED for NEOPIXEL Animations

// Apply a brightness level (index into BRIGHTNESS_LEVELS) to the LED strip.
// Defined here so the Presets module can restore brightness on patch load
// without pulling in the NeoPixel dependency. No-op when LEDs are disabled.
void applyStripBrightness(int level)
{
#ifdef NEOPIXEL_ENABLED
  if (level < 0) level = 0;
  if (level >= BRIGHTNESS_LEVEL_COUNT) level = BRIGHTNESS_LEVEL_COUNT - 1;
  strip.setBrightness(BRIGHTNESS_LEVELS[level]);
#else
  (void)level;
#endif
}



// Setup IMU Double Tap Interrupt gesture
#define int1Pin PIN_LSM6DS3TR_C_INT1
bool DoubleTapState;   // the current state of button
volatile bool doubleTapBatteryCheckPending = false; // Set by ISR, serviced in loop()


// Initiate BLE MIDI
BLEDis bledis;
BLEMidi blemidi;

// Create a new instance of the Arduino MIDI Library, and attach BluefruitLE MIDI as the transport.
MIDI_CREATE_BLE_INSTANCE(blemidi);

// -----------------------------------------------------------------------------
// MIDI Handling
// -----------------------------------------------------------------------------


// MIDI defaults
unsigned int MIDIchannel = 1; // MIDI Channel
unsigned int Keys[12]; // Assign an array to hold the notes for each key
unsigned int RootNote = 60; // Rootnote for 
unsigned int Spread = 1;
signed int RootNoteOffset = 0;
unsigned int IndexLatch = 0;
unsigned int harmonizedNote;
unsigned int Scale = 0; // Musical scale index (0 = no quantization, 1-19 = named scales)
const int FSR_AFTERTOUCH_THRESHOLD = 50; // raw ADC counts; below this = treat as no pressure

// Octave shift. octave indexes into OCTAVE_OFFSETS[]; default 3 = centered (no shift).
// octave 0 -> -36 semitones (3 octaves down)
// octave 1 -> -24
// octave 2 -> -12
// octave 3 ->   0  (default)
// octave 4 -> +12
// octave 5 -> +24
// octave 6 -> +36 semitones (3 octaves up)
extern const signed int OCTAVE_OFFSETS[7] = {-36, -24, -12, 0, 12, 24, 36};
extern const int OCTAVE_DEFAULT = 3;
extern const int OCTAVE_MAX_INDEX = 6;
int octave = OCTAVE_DEFAULT;

// keyTranspose: direct semitone shift applied to the output pitch AFTER scale
// quantization. Adjusted by channels 3↑ / 6↓ during tap-tempo mode.
// Centre LED shows WHITE in the tap-tempo display. Clamped to [-5..+5].
extern const int KEY_TRANSPOSE_MIN = -5;
extern const int KEY_TRANSPOSE_MAX =  5;
int keyTranspose = 0;

// rootTranspose: rotates the scale DEGREES by N positions (mode rotation).
// The root pitch class stays fixed (finger 0 still plays RootNote), but the
// intervals between fingers come from a rotated version of the scale's degree
// set. Rotating C major by +1 yields Dorian intervals starting on C; +2 yields
// Phrygian; etc. No effect on Scale 0 (chromatic). Range [-5..+5]; wraps
// internally so values beyond the scale length still produce a valid rotation.
// Adjusted by channels 9↑ / 12↓ during tap-tempo mode. Centre LED shows BLUE
// in the tap-tempo display.
extern const int ROOT_TRANSPOSE_MIN = -5;
extern const int ROOT_TRANSPOSE_MAX =  5;
int rootTranspose = 0;

// Tracks which transpose sub-mode was last used during tap-tempo, to set the
// colour of LED 3 (tempo blink): white = keyTranspose, blue = rootTranspose.
// 0 = neither touched yet (default white), 1 = keyTranspose, 2 = rootTranspose.
int tapTempoTransposeMode = 0;

// --- Flex-range editor (sub-menu of tap-tempo, entered by holding ch5 1s) ----
// While active, ch3/ch6 raise/lower maxNote (bent end) and ch9/ch12 raise/lower
// minNote (extended end). Flex notes remain playable so the range can be heard.
// Auto-exits after FLEX_RANGE_TIMEOUT_MS of no edit press, or by re-holding ch5.
// flexRangeEditTarget tints the numeric LED readout: 1 = min (blue), 2 = max
// (red); 0 = nothing touched yet (defaults to showing max).
bool flexRangeEditActive = false;
int  flexRangeEditTarget = 0;          // 0=none, 1=min, 2=max
const unsigned long FLEX_RANGE_TIMEOUT_MS = 5000;
unsigned long lastFlexRangePressMs = 0;
const int FLEX_NOTE_MIN = 0;           // absolute MIDI bounds for the endpoints
const int FLEX_NOTE_MAX = 127;

// Octave LED display: when an octave button is pressed, show the indicator on
// the strip for this duration, suppressing all other animations.
extern const unsigned long OCTAVE_DISPLAY_MS = 700;
unsigned long octaveDisplayStartMs = 0;
bool octaveDisplayActive = false;

// Spread / Scale LED displays. Same 700 ms window as octave display.
// Both override and black out all finger animations while active.
extern const unsigned long SPREAD_DISPLAY_MS = 700;
extern const unsigned long SCALE_DISPLAY_MS = 700;
unsigned long spreadDisplayStartMs = 0;
unsigned long scaleDisplayStartMs = 0;
bool spreadDisplayActive = false;
bool scaleDisplayActive = false;

// Note-playback quantization mode. Cycled by ch12 in preset-browser mode.
// Enum NoteQuantizeMode is defined in LedDisplay.h (shared with the LED module).
int noteQuantizeMode = NQ_OFF;
extern const unsigned long QUANTIZE_DISPLAY_MS = 700;
unsigned long quantizeDisplayStartMs = 0;
bool quantizeDisplayActive = false;
// Flex-quantize-mode indicator (cyan), shown momentarily when ch9 toggles it.
unsigned long flexQuantizeDisplayStartMs = 0;
bool flexQuantizeDisplayActive = false;

// Note quantize grid. Shared across all finger channels so simultaneous
// presses within a window fire as a chord at the same tick.
unsigned long noteQuantizeNextGridMs = 0; // Next grid tick timestamp
// Per-channel pending NoteOn slot. -1 = no NoteOn pending.
int pendingNoteChannel[16];
bool pendingNotePresent[16] = {false};

// -----------------------------------------------------------------------------
// Battery monitoring (Seeed XIAO nRF52840 Sense)
// -----------------------------------------------------------------------------
#define BAT_CHARGE_STATE 23 // LOW for charging, HIGH not charging
#define VBAT_PER_LBS (0.003515625F) // 3.6V reference / 1024 (10-bit ADC) -> volts per LSB

class Xiao {
public:
  Xiao() {} // No hardware access in ctor (runs before Arduino core init).
  void begin();
  float GetBatteryVoltage();
  bool IsChargingBattery();
};

void Xiao::begin()
{
  pinMode(VBAT_ENABLE, OUTPUT);
  pinMode(BAT_CHARGE_STATE, INPUT);
}

float Xiao::GetBatteryVoltage()
{
  digitalWrite(VBAT_ENABLE, LOW);
  uint32_t adcCount = analogRead(PIN_VBAT);
  float adcVoltage = adcCount * VBAT_PER_LBS;
  // Voltage divider correction. The nominal Seeed XIAO schematic ratio is
  // 1510/510 = 2.9608, but calibration measurements show a consistent +280 mV
  // offset, yielding a corrected multiplier of 3.183.
  // Calibration data:
  //   reported 3.68 V -> actual 3.96 V (delta +280 mV)
  //   reported 3.79 V -> actual 4.07 V (delta +280 mV)
  // corrected = nominal * (actual / reported) = 2.9608 * (3.96 / 3.68) = 3.183
  float vBat = adcVoltage * 3.183f;
  digitalWrite(VBAT_ENABLE, HIGH);
  return vBat;
}

bool Xiao::IsChargingBattery()
{
  return digitalRead(BAT_CHARGE_STATE) == LOW;
}

Xiao xiao; // Single global instance; call xiao.begin() from setup().

// Battery boot indicator state. Captured once at boot; the strip displays the
// indicator for BATTERY_DISPLAY_MS before yielding to normal animations.
extern const unsigned long BATTERY_DISPLAY_MS = 2000;
extern const unsigned long BATTERY_BLINK_MS = 125; // 125 on / 125 off ~= 4 Hz
unsigned long batteryDisplayStartMs = 0;
bool batteryDisplayActive = false;
float batteryBootVoltage = 0.0f;
bool batteryBootCharging = false;
int batteryBootBars = 0; // 0..7

// -----------------------------------------------------------------------------
// Accelerometer X/Y handling
// -----------------------------------------------------------------------------

// Accelerometer variables
float rawAccelX;                          // Raw IMU data X
float rawAccelY;                          // Raw IMU data Y
float rawAccelZ;                          // Raw IMU data Z
unsigned int AccelX;                      // Scaled IMU data X
unsigned int AccelY;                      // Scaled IMU data Y
// - unsigned int AccelZ;                 // Z axis - not needed for MIDI CC (yet)
unsigned int lastAccelX;                  // Previous IMU reading
unsigned int lastAccelY;
// - unsigned int lastAccelZ;             // Z axis - not needed for MIDI CC (yet)
unsigned long lastExecutionTimeAccel = 0; // Timer for sending IMU MIDI CC's
const unsigned long intervalAccel = 25;   // Send IMU MIDI CC's every X ms
unsigned int CCAccelX = 1;               // Default: Set IMU X axis to MIDI CC 1 (mod wheel)
unsigned int CCAccelY = 11;                // Default: Set IMU Y axis to MIDI CC 11 (expression)
// CC mapping swap (toggled by ch4 in the tap-tempo menu). When true, CCAccelX
// and CCAccelY are exchanged so Y sends CC1 and X sends CC11.
bool ccSwapped = false;


// -----------------------------------------------------------------------------
// Flex and FSR analog input handling
// -----------------------------------------------------------------------------


// Temporary variables for processing analog inputs (Index finger Flex sensor and Thumb FSR pressure sensor)
int indexFLEX = 2;                        // analog pin A2
int thumbFSR = 1;                         // analog pin A1
int lastFlexVal;                          // track Flex sensor values read from pin A2
int lastFsrVal;                           // track FSR sensor values read from pin A1

// Index-finger flex-to-note settings. When the index modal contact (channel 2)
// is held, the index finger's flex sensor maps to MIDI notes in the range
// [FLEX_MIN_NOTE..FLEX_MAX_NOTE]. The raw flex reading is expected in the
// range [FLEX_RAW_MIN..FLEX_RAW_MAX]; readings outside that range clamp.
//
// CALIBRATION: this sensor reads HIGHER when extended, LOWER when bent --
// so FLEX_RAW_MIN corresponds to the FULLY BENT position (highest note,
// most LEDs lit), and FLEX_RAW_MAX corresponds to the FULLY EXTENDED
// position (lowest note, no LEDs lit).
const int FLEX_RAW_MIN = 85;   // Fully bent
const int FLEX_RAW_MAX = 190;  // Fully extended
int minNote = 48;        // MIDI note when fully extended (raw = FLEX_RAW_MAX)
int maxNote = 84;        // MIDI note when fully bent    (raw = FLEX_RAW_MIN)
int lastFlexNote = -1;   // Last MIDI note sent via flex (-1 = none active)
const unsigned long FLEX_SAMPLE_INTERVAL_MS = 10; // Don't sample faster than this
unsigned long lastFlexSampleMs = 0;

// Flex-note quantization subdivision mode. Cycled in preset-browser mode via
// channel 9. Enum FlexQuantizeMode is defined in LedDisplay.h.
int flexQuantizeMode = FQ_1_16;

// Flex note quantization. New flex note values are deferred until the next
// grid boundary; this gives a steady rhythmic feel to the flex sweep.
// Common values at 120 BPM:
//   1/32 note = 62 ms
//   1/16 note = 125 ms  (default)
//   1/8  note = 250 ms
//   1/4  note = 500 ms
// For other tempi, ms = 60000 / BPM / subdivisions_per_beat.
unsigned long flexQuantizeMs = 125;
unsigned long flexNextGridMs = 0; // Timestamp of next grid tick (millis())
int pendingFlexNote = -1;         // Note value waiting to be sent at next tick (-1 = none)
bool pendingFlexNoteOff = false;  // True when a NoteOff is queued for next tick

// Pinky palm (channel 15) press tracking. A short press toggles Spread on
// release; a hold of >= PINKY_SCALE_HOLD_MS cycles the Scale once at the
// 1 s mark and suppresses the on-release Spread toggle.
const unsigned long PINKY_SCALE_HOLD_MS = 1000;
unsigned long pinkyPressedMs = 0;
bool pinkyScaleFired = false; // True if the 1s hold action already fired this hold
// Scale-edit latch. Toggled ON by a 1s pinky-palm hold; persists after the
// palm is released so the thumb is free to tap ch3 (scale up) / ch6 (scale
// down). A subsequent pinky-palm hold toggles it OFF. While active, ch3 and
// ch6 are intercepted for scale scrolling; all other note channels still play
// normally so the chosen scale can be auditioned before exiting.
bool scaleEditActive = false;
// Auto-exit: scale-edit mode leaves automatically if no scale-select button
// (ch3/ch6) has been pressed for SCALE_EDIT_TIMEOUT_MS.
const unsigned long SCALE_EDIT_TIMEOUT_MS = 3000;
unsigned long lastScaleEditPressMs = 0;

// Index modal (channel 2) held-state flag. accelRead() gates AccelY by this.
bool indexHeld = false;

// Flex bar LED display state. Only visible while indexHeld is true.
//   flexBarBend     = normalized 0..1 bend value (0 = least bent, 1 = fully bent)
//   flexPulseStartMs = millis() of the most recent note-triggered white pulse
//   flexPulseBaseW   = W-channel intensity to add at pulse start (decays to 0)
// Multiple note triggers within the 100 ms pulse window add their pulse
// intensities, capped at 255 in the renderer.
float flexBarBend = 0.0f;
extern const unsigned long FLEX_PULSE_MS = 100;
const uint8_t FLEX_PULSE_W_PER_TRIGGER = 128; // 50% of 255
unsigned long flexPulseStartMs = 0;
float flexPulseBaseW = 0.0f; // accumulator for stacked pulses (float so it can decay smoothly)

// Global tempo (BPM). Drives flex-note quantization and the tap-tempo blink.
// Settable via the tap-tempo feature (octave-up hold + 8 taps), or via
// incoming MIDI clock (which takes precedence when active).
extern const int TEMPO_MIN = 40;
extern const int TEMPO_MAX = 240;
int tempo = 120;


// FSR Aftertouch settings (velocity sensitivity removed - using fixed velocity 100)
const int FSR_RAW_MAX = 550;                     // Max expected raw ADC value from the thumb FSR
const unsigned long AFTERTOUCH_DELAY_MS = 30;   // Wait this long after NoteOn before sending aftertouch
const unsigned long AFTERTOUCH_INTERVAL_MS = 20; // Min interval between aftertouch updates

// Per-note aftertouch tracking. A note is "active" while its contact is held.
// noteOnTime[ch] = millis() of the NoteOn for channel ch (used to gate the 50 ms delay).
unsigned long noteOnTime[16] = {0};
bool noteActive[16] = {false};            // Currently held (NoteOn sent, NoteOff not yet)
// Pitch sent in the most recent NoteOn for each channel. NoteOff sends the
// SAME pitch back so that Scale / Spread / Octave changes between press and
// release can't strand a different MIDI note on the synth.
byte noteActivePitch[16] = {0};
unsigned long lastAftertouchSendTime = 0; // Last time any aftertouch was sent
int lastAftertouchValue = -1;             // Last aftertouch value sent (-1 = none yet)

// -----------------------------------------------------------------------------
// Tempo-tracking pitch bend (preset feature, toggled by ch8 in the browser).
//
// When enabled, once per second the live incoming MIDI-clock tempo is compared
// (as a rounded integer BPM) to the preset's saved "project" tempo. If they
// differ, a MIDI pitch-bend message is sent to retune a sample/synth so it
// plays in tune at the new tempo. Uses the TempoPitchShifter module.
//
// projectTempo is the reference set when a preset is saved or loaded (the tempo
// the sample was recorded at). When no preset has been loaded it tracks the
// current tempo so there is no spurious bend.
bool  tempoBendEnabled = false;
int   projectTempo = 120;            // saved reference tempo (rounded BPM)
TempoPitchShifter tempoShifter;      // default +/- 4 semitone bend range
extern const unsigned long TEMPO_BEND_INTERVAL_MS = 1000;
unsigned long lastTempoBendMs = 0;
int   lastSentBendTempo = -1;        // last incoming tempo we acted on (debounce)

// setTempo() now lives in TempoControl.{h,cpp} (clamps to TEMPO_MIN/MAX and
// re-derives the flex-quantize grid via ScaleQuant's flexQuantizeIntervalMs).

// -----------------------------------------------------------------------------
// Preset (patch) browser mode.
//
// Entered by a 1 s octave-down hold (replacing the old quantize-cycle gesture).
// While active:
//   - ch3 / ch6  : increment / decrement the selected preset slot (with hold-
//                  repeat). No patch is loaded until the flex modal is touched.
//   - ch9        : cycle flex quantization mode (OFF / 1/8 / 1/16 / 1/32)
//   - ch12       : cycle note quantization mode (OFF / 1/8 / 1/16 / 1/32)
//   - ch2 (flex modal touch) : LOAD the selected preset and exit browser mode
//   - pinky palm (ch15) 1 s hold : SAVE current settings to the selected slot
//   - octave-down (ch1) 1 s hold : exit WITHOUT load/save (a newly chosen
//                                  quantization mode is kept)
//   - other note channels (4,5,7-14) remain playable for auditioning
//   - the flex sensor note path is disabled while browsing (indexHeld forced off)
//
// The preset indicator is shown as one of 63 color bands: 7 LED-count steps x
// 9 colors (purple, pink, blue, cyan, green, yellow, orange, red, white-via-W).
extern const int PRESET_COUNT = 63;
bool presetBrowserActive = false;
int selectedPreset = 0; // 0..62
// Which sub-view the preset browser is currently showing on the strip:
//   0 = preset color band (default on entry; shown when ch3/ch6 are used)
//   1 = quantize display (shown when ch9/ch12 are used)
int presetSubView = 0;

// Incoming MIDI Program Change -> preset load. The PC handler runs in the BLE
// scheduler task, so it only sets these flags; the main loop performs the load
// and triggers the brief LED override. PC numbers 0..62 map to presets 0..62;
// higher PC numbers are ignored.
volatile bool pendingPcLoad = false;
volatile int  pendingPcSlot = 0;

// Brief preset-bar LED override shown when a PC is received (overrides all other
// LED states for PC_PRESET_DISPLAY_MS).
extern const unsigned long PC_PRESET_DISPLAY_MS = 1200;
unsigned long pcPresetDisplayStartMs = 0;
bool pcPresetDisplayActive = false;
int  pcPresetDisplaySlot = 0; // which slot's bar to draw during the override

// Brief confirmation flash shown when a patch is saved or loaded: a 300 ms
// gradient bar across the strip. Save = orange->cyan, Load = green->cyan.
extern const unsigned long PATCH_CONFIRM_MS = 300;
unsigned long patchConfirmStartMs = 0;
bool patchConfirmActive = false;
int  patchConfirmKind = 0; // 0 = save (orange->cyan), 1 = load (green->cyan)

// ch3 / ch6 hold-repeat scrolling of the preset indicator while browsing.
const unsigned long PRESET_SCROLL_REPEAT_DELAY_MS = 400;    // initial hold before repeat
const unsigned long PRESET_SCROLL_REPEAT_INTERVAL_MS = 150; // interval between repeats
unsigned long presetScrollPressedMs[16] = {0};
unsigned long presetScrollLastStepMs[16] = {0};

// -----------------------------------------------------------------------------
// Preset persistence (Patch struct, PATCH_MAGIC/VERSION, FACTORY_RESET), the
// save/load/apply path, the patch-confirm trigger, and the serial patch console
// now live in Presets.{h,cpp}. NoteSpread() lives in ScaleQuant.{h,cpp}.
// The shared globals those modules read/write are defined here in main.cpp and
// declared extern via GloveState.h / Presets.h.
// -----------------------------------------------------------------------------


// -----------------------------------------------------------------------------
// Tempo handling
// -----------------------------------------------------------------------------
extern const float CLOCK_EMA_ALPHA = 0.1f;            // Smoothing factor (smaller = smoother)
extern const unsigned long CLOCK_TIMEOUT_MS = 2000;   // If no clock for this long, drop external lock
extern const int CLOCK_MIN_SAMPLES = 8;               // Need this many pulses before we trust the EMA
extern const int CLOCK_PPQN = 24;                     // MIDI standard
unsigned long lastClockMs = 0;                 // Timestamp of most recent clock pulse
float clockEmaIntervalMs = 0.0f;               // EMA of inter-pulse interval (ms)
int clockSampleCount = 0;
bool externalClockActive = false;              // True when locked to incoming clock

// "Tap tempo unavailable" orange-flash state.
// When externalClockActive is true and the user holds octave-up for 1 s, all
// LEDs flash orange instead of entering tap-tempo mode.
extern const unsigned long ORANGE_FLASH_MS = 500;
unsigned long orangeFlashEndMs = 0;
bool orangeFlashActive = false;

// Octave button press tracking. Octave change now fires on RELEASE (not press).
// Octave-up additionally has a 1-second hold gesture to enter tap-tempo mode
// (or trigger the orange flash if external clock is active).
const unsigned long OCTAVE_UP_HOLD_MS = 1000;
unsigned long octaveUpPressedMs = 0;
bool octaveUpHoldFired = false; // True if hold-action already fired this hold
// ch5 hold (in tap-tempo): 1 s hold toggles the flex-range editor.
const unsigned long CH5_HOLD_MS = 1000;
unsigned long ch5PressedMs = 0;
bool ch5HoldFired = false;
unsigned long octaveDownPressedMs = 0;
bool octaveDownHoldFired = false; // True if hold-action already fired this hold
const unsigned long OCTAVE_DOWN_HOLD_MS = 1000;


// Tap-tempo state machine.
//   - On entry: tapTempoActive = true, blank everything else, start blinking
//     LEDs 0 and 6 white at the current tempo.
//   - Each subsequent octave-up press is recorded as a tap and lights LED 4
//     briefly. After 8 taps, the BPM is computed from the average of the 7
//     intervals.
//   - 5 s with no tap -> cancel without changing tempo.
//   - After completion: show LEDs 0/6 blinking at the NEW tempo for a brief
//     confirmation window, then exit.
extern const int TAP_TEMPO_TAPS_REQUIRED = 8;
extern const unsigned long TAP_TEMPO_TIMEOUT_MS = 5000;
extern const unsigned long TAP_TEMPO_CONFIRM_MS = 1500;    // Confirmation blink time after 8th tap
extern const unsigned long TAP_TEMPO_LED4_FLASH_MS = 100;  // LED 4 flash per tap
extern const unsigned long TAP_BLINK_MIN_PERIOD_MS = 100;  // Sanity floor for blink period
bool tapTempoActive = false;
unsigned long tapTimes[TAP_TEMPO_TAPS_REQUIRED] = {0};
int tapCount = 0;
unsigned long lastTapMs = 0;
unsigned long led4FlashUntilMs = 0;
bool tapTempoConfirming = false;          // True after 8th tap, during confirmation
unsigned long tapTempoConfirmEndMs = 0;


// -----------------------------------------------------------------------------
// Stuck note handling: NoteOff retry (BLE stuck-note prevention) 
//
// BLE-MIDI does not guarantee delivery, so a dropped NoteOff leaves a stuck
// note. For every finger-note NoteOff we schedule one repeat ~150 ms later;
// the repeat is only sent if NO finger notes are currently pressed (so it can
// never cut off an intentionally held/re-pressed note). Flex notes are
// excluded -- they have their own immediate handling and work reliably.
const unsigned long NOTE_OFF_RETRY_MS = 150;
bool          noteOffRetryPending[16] = {false};
byte          noteOffRetryPitch[16]   = {0};
unsigned long noteOffRetryDueMs[16]   = {0};

// --- Idle MIDI panic (All Notes Off) -------------------------------------
// As a final stuck-note safeguard, send All Notes Off (CC123, value 0) once a
// second whenever the instrument is fully idle -- no finger notes pressed, no
// flex note sounding, and the flex modal not engaged. Harmless when nothing is
// stuck; clears anything a dropped NoteOff might have left hanging.
const unsigned long PANIC_INTERVAL_MS = 1000;
const uint8_t MIDI_CC_ALL_NOTES_OFF = 123;
unsigned long lastPanicMs = 0;

// -----------------------------------------------------------------------------
// Note handling and MIDI output: Reading glove channel 3-14 mux inputs 
// -----------------------------------------------------------------------------
//
// Set variables for reading 4067 inputs and triggering MIDI notes
CD74HC4067 mux(9, 8, 7, 6); // 4067 Pins for S0, S1, S2 and S3
const int inputPin = 3;
// const int muxEnablePin = 10;
const int firstChannel = 0; // First 4067 Channel to demux / scan from
const int numChannels =
    16; // Last 4067 Channel to scan from (assuming all inputs are connected sequentially, eg. 1-15)
const int debounceDelay = 5;                 // Debounce delay in milliseconds
const int noteOffDelay = 10;                  // Delay for Note Off in milliseconds
unsigned long lastDebounceTime[numChannels]; // Debounce Timer
bool Buttons[numChannels];
bool prevButtons[numChannels];
bool noteOffFlag[numChannels];
bool noteStates[127] = {false}; // keep track of the play state of each note

// Create a instance of class LSM6DS3
LSM6DS3 myIMU(I2C_MODE, 0x6A); // I2C device address 0x6A
uint16_t errorsAndWarnings = 0;

// Read 4067 inputs
int digitalReadFromMultiplexer(int channel)
{ // Read each 4067 channel input
  mux.channel(channel);
  return digitalRead(inputPin);
}

// Immediate NoteOn (no quantization): send the MIDI note, mark active for
// aftertouch, fire the LED ripple. Used directly when quantize mode is OFF,
// and called from FlushPendingNotes() when a queued NoteOn's tick arrives.
static void NoteOnImmediate(int channel)
{
  // Convert channel (3..14) to a 0-based button position (0=lowest/pinky base,
  // 11=highest/index tip), then derive pitch via scale-degree mapping.
  // When Scale == 0, falls back to Keys[]/spread path (no deduplication needed).
  // keyTranspose is applied as a raw semitone shift after scale mapping.
  int buttonPos = 14 - channel; // 0..11
  int p = scaleDegreeToMidiPitch(buttonPos) + keyTranspose;
  if (p < 0) p = 0;
  if (p > 127) p = 127;
  byte pitch = (byte)p;

  DBG_PRINT("NoteOn - Channel ");
  DBG_PRINT(channel);
  DBG_PRINT(" pos=");
  DBG_PRINT(buttonPos);
  DBG_PRINT(" pitch=");
  DBG_PRINT(pitch);
  DBG_PRINT(" scale=");
  DBG_PRINT(Scale);
  DBG_PRINT(" oct=");
  DBG_PRINTLN(octave);

  MIDI.sendNoteOn(pitch, 100, MIDIchannel);

  // A fresh NoteOn on this channel cancels any pending stuck-note retry.
  noteOffRetryPending[channel] = false;

  // Track this note for aftertouch gating and for symmetric NoteOff.
  noteOnTime[channel] = millis();
  noteActive[channel] = true;
  noteActivePitch[channel] = pitch;

#ifdef NEOPIXEL_ENABLED
  TriggerFingerNoteOn(channel);
#endif
}

// Send MIDI note on command. Fixed velocity 100 (FSR velocity sensitivity removed).
// Note order: channel 14 = lowest (Keys[0]) at pinky base; channel 3 = highest
// (Keys[11]) at index tip. Final pitch is offset by the current octave shift.
//
// If noteQuantizeMode != NQ_OFF, the NoteOn (and its LED ripple) is deferred to
// the next note-quantize grid tick. NoteOff is always immediate.
void NoteOn(int channel)
{
  if (noteQuantizeMode == NQ_OFF) {
    NoteOnImmediate(channel);
    return;
  }

  // Quantized: queue for next tick. Per-channel slot so simultaneous presses
  // on different fingers all fire as a chord. If a NoteOn was already queued
  // on this channel (very fast retap), the new one just overwrites; the old
  // queued press is silently dropped (no audible note ever played for it).
  pendingNoteChannel[channel] = channel;
  pendingNotePresent[channel] = true;
  DBG_PRINT("NoteOn QUEUED - Channel ");
  DBG_PRINTLN(channel);
}

// Send MIDI note off command. Always immediate (NoteOff is never quantized).
// Sends the SAME pitch that NoteOn sent for this channel (stored in
// noteActivePitch[]) -- this is critical because Scale, Spread, and Octave
// can change between press and release, and recomputing the pitch at NoteOff
// time would otherwise send a NoteOff for a different note than was played.
//
// Also cancels any pending NoteOn for this channel: if the user presses and
// releases within one quantize window, the press is silently dropped so the
// synth never sees an unmatched NoteOff or a stranded NoteOn.
void NoteOff(int channel)
{
  // Cancel any pending NoteOn for this channel before sending the off.
  bool wasPending = pendingNotePresent[channel];
  pendingNotePresent[channel] = false;

  if (wasPending && !noteActive[channel]) {
    // The NoteOn never fired -- nothing to release. Silently drop the off.
    DBG_PRINT("NoteOff cancelled (pending NoteOn never fired) - Channel ");
    DBG_PRINTLN(channel);
    return;
  }

  if (!noteActive[channel]) {
    // No active note on this channel; nothing to release.
    return;
  }

  byte pitch = noteActivePitch[channel];

  DBG_PRINT("NoteOff - Channel ");
  DBG_PRINT(channel);
  DBG_PRINT(" pitch=");
  DBG_PRINTLN(pitch);

  MIDI.sendNoteOff(pitch, 0, MIDIchannel);

  // Schedule a single retry ~150 ms later in case BLE dropped this NoteOff.
  // It will only actually re-send if no finger notes are pressed at that time.
  noteOffRetryPitch[channel]   = pitch;
  noteOffRetryDueMs[channel]   = millis() + NOTE_OFF_RETRY_MS;
  noteOffRetryPending[channel] = true;

  // Clear active flag for this note
  noteActive[channel] = false;

#ifdef NEOPIXEL_ENABLED
  TriggerFingerNoteOff(channel);
#endif

  // If no notes are active anymore, send a zero-aftertouch to release any held pressure
  // and reset the tracking value so the next note starts clean.
  bool anyActive = false;
  for (int i = 0; i < 16; i++) {
    if (noteActive[i]) {
      anyActive = true;
      break;
    }
  }
  if (!anyActive && lastAftertouchValue > 0) {
    MIDI.sendAfterTouch(0, MIDIchannel);
    lastAftertouchValue = 0;
  }
}

// Forward declaration (defined further down).
static bool anyNotePressed();

// Service the BLE stuck-note retry. Called every loop. For each channel with a
// pending retry whose time has come, re-send the NoteOff -- but ONLY if no
// finger notes are currently pressed, so an intentionally held or re-pressed
// note is never cut. Each NoteOff schedules exactly one retry; it fires once
// and clears (or is cleared early by a new NoteOn on that channel).
void UpdateNoteOffRetries()
{
  unsigned long now = millis();
  bool notesDown = anyNotePressed();
  for (int ch = 0; ch < 16; ch++) {
    if (!noteOffRetryPending[ch]) continue;
    if ((long)(now - noteOffRetryDueMs[ch]) < 0) continue; // not due yet
    // Due now. Only re-send when nothing is being held, to be safe.
    if (!notesDown) {
      MIDI.sendNoteOff(noteOffRetryPitch[ch], 0, MIDIchannel);
      MIDI.sendNoteOff(noteOffRetryPitch[ch], 0, MIDIchannel); // Send Note Off a second time!
      MIDI.sendNoteOff(noteOffRetryPitch[ch], 0, MIDIchannel); // Send Note Off a third time!
      DBG_PRINT("NoteOff RETRY - Channel ");
      DBG_PRINT(ch);
      DBG_PRINT(" pitch=");
      DBG_PRINTLN(noteOffRetryPitch[ch]);
    }
    // One-shot: clear whether or not we sent (if notes are down, the active
    // note's own release will schedule a fresh retry later).
    noteOffRetryPending[ch] = false;
  }
}

// Send All Notes Off (CC123 = 0) once per second while fully idle, as a final
// stuck-note safeguard. Gated so it can never cut a sounding note: requires no
// finger notes pressed, no flex note active, and the flex modal not engaged.
//
// Note: not all MIDI devices will listen to this command
//
void UpdateIdlePanic()
{
  unsigned long now = millis();
  if ((now - lastPanicMs) < PANIC_INTERVAL_MS) return;
  lastPanicMs = now;

  if (anyNotePressed() || lastFlexNote >= 0 || indexHeld) return; // not idle

  MIDI.sendControlChange(MIDI_CC_ALL_NOTES_OFF, 0, MIDIchannel);
}

// -----------------------------------------------------------------------------
// MIDI Aftertouch handling
// -----------------------------------------------------------------------------
//
// Send channel aftertouch based on current FSR reading, but only for notes that
// have been held for at least AFTERTOUCH_DELAY_MS, and not more often than
// AFTERTOUCH_INTERVAL_MS, and only when the value actually changes.
void UpdateAftertouch()
{
  unsigned long now = millis();

  // Rate limit
  if ((now - lastAftertouchSendTime) < AFTERTOUCH_INTERVAL_MS)
    return;

  // Is at least one note held long enough to be in aftertouch territory?
  // Also eligible when a flex note is active (lastFlexNote >= 0).
  bool eligible = (lastFlexNote >= 0);
  if (!eligible) {
    for (int i = 0; i < 16; i++) {
      if (noteActive[i] && (now - noteOnTime[i]) >= AFTERTOUCH_DELAY_MS) {
        eligible = true;
        break;
      }
    }
  }
  if (!eligible)
    return;

int raw = analogRead(thumbFSR);              // read FSR sensor values from pin A2
if (raw < FSR_AFTERTOUCH_THRESHOLD) raw = 0; // ignore noise floor
raw = constrain(raw, 0, FSR_RAW_MAX);
int at = map(raw, 0, FSR_RAW_MAX, 0, 127);
at = constrain(at, 0, 127);

  if (at != lastAftertouchValue) {
    MIDI.sendAfterTouch((byte)at, MIDIchannel); // Send MIDI channel aftertouch (CAT) from FSR values
    lastAftertouchValue = at;
    lastAftertouchSendTime = now;
    // DBG_PRINT("AT=");
    // DBG_PRINTLN(at);
  }
}

// -----------------------------------------------------------------------------
// Process incoming MIDI messages
// -----------------------------------------------------------------------------
void midiRead()
{

  // Don't continue if we aren't connected.
  if (!Bluefruit.connected()) {
    return;
  }

  // Don't continue if the connected device isn't ready to receive messages.
  if (!blemidi.notifyEnabled()) {
    return;
  }

  // read any new MIDI messages
  MIDI.read();
}

// Initialize BLE
void startAdv(void)
{

  // Set General Discoverable Mode flag
  Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);

  // Advertise TX Power
  Bluefruit.Advertising.addTxPower();

  // Advertise BLE MIDI Service
  Bluefruit.Advertising.addService(blemidi);

  // Secondary Scan Response packet (optional)
  Bluefruit.ScanResponse.addName();

  // Start Advertising
  Bluefruit.Advertising.restartOnDisconnect(true);
  Bluefruit.Advertising.setInterval(32, 244); // in unit of 0.625 ms
  Bluefruit.Advertising.setFastTimeout(30);   // number of seconds in fast mode
  Bluefruit.Advertising.start(0);             // 0 = Don't stop advertising after n seconds
}

  // Process incoming MIDI note on messages
  void handleNoteOn(byte channel, byte pitch, byte velocity)
{

  // Log when a note is pressed.
  DBG_PRINTF("Note on: channel = %d, pitch = %d, velocity - %d", channel, pitch, velocity);
  DBG_PRINTLN();
}

  // Process incoming MIDI note off messages
  void handleNoteOff(byte channel, byte pitch, byte velocity)
{

  // Log when a note is released.
  DBG_PRINTF("Note off: channel = %d, pitch = %d, velocity - %d", channel, pitch, velocity);
  DBG_PRINTLN();
}

  // Incoming MIDI real-time clock pulse (0xF8). Called at 24 PPQN by the
  // MIDI library from the BLE-MIDI scheduler task. Keep this FAST -- no Serial
  // I/O, no allocation. Just timestamp and update the EMA.
  void handleClock()
{
    unsigned long now = millis();
    if (lastClockMs != 0) {
      unsigned long interval = now - lastClockMs;
      if (interval > 0 && interval < 200) {
        // Plausible inter-pulse interval (200 ms / 24 PPQN ~= 12.5 BPM lower bound).
        if (clockSampleCount == 0) {
          clockEmaIntervalMs = (float)interval;
        } else {
          clockEmaIntervalMs = (CLOCK_EMA_ALPHA * (float)interval) +
                               ((1.0f - CLOCK_EMA_ALPHA) * clockEmaIntervalMs);
        }
        if (clockSampleCount < 32767) clockSampleCount++;
      }
    }
    lastClockMs = now;
}


  // Incoming MIDI Program Change. Runs in the BLE scheduler task, so it must be
  // fast: just record the requested slot and flag it for the main loop, which
  // performs the actual patch load and LED override. PC value 0..62 -> preset
  // 0..62; out-of-range values are ignored.
  void handleProgramChange(byte channel, byte number)
{
    (void)channel;
    if (number < PRESET_COUNT) {
      pendingPcSlot = (int)number;
      pendingPcLoad = true;
    }
}

  // Called from loop() to:
  //   - Decide whether the external clock is "active" (locked or timed out)
  //   - Apply the EMA-derived BPM to setTempo() when locked and BPM has changed
  //     by >= 1 BPM (avoids constant nudging on sub-BPM jitter)
  void updateMidiClockState()
{
    unsigned long now = millis();
  
    // Timeout: no clock pulse for too long -> drop external lock.
    if (externalClockActive && lastClockMs != 0 && (now - lastClockMs) >= CLOCK_TIMEOUT_MS) {
      externalClockActive = false;
      clockSampleCount = 0;
      clockEmaIntervalMs = 0.0f;
      DBG_PRINTLN("MIDI clock timeout -- internal tempo re-enabled");
      // Note: we don't change `tempo` here; the last externally-set BPM stays
      // until the user changes it via tap-tempo (which is now re-enabled).
      return;
  }

  // Lock-on once we have enough samples AND we're not already locked.
    if (!externalClockActive && clockSampleCount >= CLOCK_MIN_SAMPLES && clockEmaIntervalMs > 0.0f) {
      externalClockActive = true;
      DBG_PRINTLN("MIDI clock detected -- tap tempo disabled");
      // Cancel any in-progress tap tempo session.
      if (tapTempoActive) {
        tapTempoActive = false;
        tapTempoConfirming = false;
        tapCount = 0;
        DBG_PRINTLN("Tap tempo cancelled by incoming MIDI clock");
    }
  }

  // While locked, recompute BPM from EMA and update if it actually changed.
  if (externalClockActive && clockEmaIntervalMs > 0.0f) {
    // BPM = 60000 / (interval_per_pulse_ms * PPQN)
    float beatMs = clockEmaIntervalMs * (float)CLOCK_PPQN;
    if (beatMs > 0.0f) {
      int newBpm = (int)(60000.0f / beatMs + 0.5f);
      if (newBpm < TEMPO_MIN) newBpm = TEMPO_MIN;
      if (newBpm > TEMPO_MAX) newBpm = TEMPO_MAX;
      if (newBpm != tempo) {
        setTempo(newBpm);
      }
    }
  }
}

// -----------------------------------------------------------------------------
// Tempo-tracking pitch bend
// -----------------------------------------------------------------------------
// Called every loop iteration; self-throttles to
// once per TEMPO_BEND_INTERVAL_MS. When enabled, compares the live incoming
// tempo (rounded integer BPM) against the saved project tempo and, if they
// differ, sends a MIDI pitch-bend computed by TempoPitchShifter. The bend is
// only re-sent when the rounded incoming tempo changes, to avoid flooding.
void UpdateTempoBend()
{
  if (!tempoBendEnabled) return;

  unsigned long now = millis();
  if ((now - lastTempoBendMs) < TEMPO_BEND_INTERVAL_MS) return;
  lastTempoBendMs = now;

  // Only meaningful while we have a live external tempo to track.
  if (!externalClockActive) return;

  int incoming = tempo;            // already rounded integer BPM
  if (incoming == lastSentBendTempo) return; // nothing changed since last send
  lastSentBendTempo = incoming;

  if (incoming == projectTempo) {
    // Back in tune with the project tempo -> centre the bend.
    MIDI.sendPitchBend(0, MIDIchannel); // 0 = centre for this MIDI library
    DBG_PRINTLN("Tempo bend: centred (matches project tempo)");
    return;
  }

  PitchBendResult res = tempoShifter.compute((float)projectTempo, (float)incoming);
  if (!res.valid) return;

  // res.midiValue is 0..16383 (centre 8192). The Arduino MIDI library's
  // sendPitchBend(int, channel) expects a signed -8192..+8191 value.
  int bendSigned = (int)res.midiValue - 8192;
  if (bendSigned < -8192) bendSigned = -8192;
  if (bendSigned > 8191)  bendSigned = 8191;
  MIDI.sendPitchBend(bendSigned, MIDIchannel);

  DBG_PRINT("Tempo bend: project ");
  DBG_PRINT(projectTempo);
  DBG_PRINT(" -> incoming ");
  DBG_PRINT(incoming);
  DBG_PRINT(" => bend ");
  DBG_PRINT(res.midiValue);
  if (res.outOfRange) DBG_PRINT(" (CLAMPED)");
  DBG_PRINTLN("");
}


// -----------------------------------------------------------------------------
// Read accelerometer and send MIDI CC's accordingly
// -----------------------------------------------------------------------------
  void accelRead()
{

  // Convert accelerometer data to 7-bit 0-127 MIDI CC data
  rawAccelX =
      constrain((myIMU.readFloatAccelY()), -8, 8) + 8; // Reverse X and Y because of IMU orientation on hand
  rawAccelY =
      constrain((myIMU.readFloatAccelX()), -8, 8) + 8; // Reverse X and Y because of IMU orientation on hand
  // rawAccelZ = constrain((myIMU.readFloatAccelZ()),-8,8) + 8; // Z axis not needed for MIDI CC for now



  AccelX = labs(constrain(round((rawAccelX) * 8), 0, 127));
  AccelX = 127 - AccelX; // reversed direction
  AccelY = labs(constrain(round((rawAccelY) * 8), 0, 127));
  // Bottom deadzone for Y: the lowest 25% of travel holds at 0, then the
  // remaining 75% rescales linearly to the full 0..127 range.
  //   raw 0..31  -> 0
  //   raw 32..127 -> 0..127
  const int ACCELY_DEADZONE = 32; // 25% of 127 ~= 32
  if ((int)AccelY <= ACCELY_DEADZONE) {
    AccelY = 0;
  } else {
    AccelY = (unsigned int)constrain(
        round(((int)AccelY - ACCELY_DEADZONE) * 127.0f / (127 - ACCELY_DEADZONE)),
        0, 127);
  }

  // AccelZ = labs(constrain(round((rawAccelZ) * 8),0,127)); // AccelZ not used for now 

  // AccelX is sent at all times whenever its value changes.
  if (AccelX != lastAccelX) {
    MIDI.sendControlChange(CCAccelX, AccelX, MIDIchannel);
    lastAccelX = AccelX;
  }

  if (AccelY != lastAccelY) {
    // AccelY is sent ONLY while the index modal contact (channel 2) is held.
    if (indexHeld) {
      MIDI.sendControlChange(CCAccelY, AccelY, MIDIchannel);
    }
    lastAccelY = AccelY;
  }
}

// -----------------------------------------------------------------------------
// Flex sensor handling
// -----------------------------------------------------------------------------
// Read the index-finger flex sensor and, if its harmonized MIDI note has
// changed since the last sample, QUEUE the new note for the next quantize
// grid tick. Flex notes are quantized to the current Scale via the harmonizer
// and shifted by the current octave, so they match the finger-pad notes.
// Direction: lowest flex (least bent) = maxNote, highest flex = minNote.
// Called continuously from loop() while the index modal contact is held.
void FlexNoteUpdate()
{
  unsigned long now = millis();
  if ((now - lastFlexSampleMs) < FLEX_SAMPLE_INTERVAL_MS) return;
  lastFlexSampleMs = now;

  int raw = analogRead(indexFLEX);

  // Periodic raw value print for calibration. Comment out once calibrated.
  // Adjust FLEX_RAW_MIN / FLEX_RAW_MAX above so the printed range when you
  // flex from fully extended to fully bent matches them.
  static unsigned long lastFlexPrintMs = 0;
  if ((now - lastFlexPrintMs) >= 250) {
    lastFlexPrintMs = now;
    DBG_PRINT("flex raw=");
    DBG_PRINTLN(raw);
  }

  int clamped = constrain(raw, FLEX_RAW_MIN, FLEX_RAW_MAX);

  // Normalized bend value for the LED bar.
  // Sensor reads LOWER when bent: FLEX_RAW_MIN (85) = fully bent, FLEX_RAW_MAX (190) = extended.
  // flexBarBend = 1.0 when fully bent (most LEDs lit), 0.0 when extended (no LEDs).
  flexBarBend = (float)(FLEX_RAW_MAX - clamped) /
                (float)(FLEX_RAW_MAX - FLEX_RAW_MIN);

  // Build the ascending set of scale-quantized notes that fall within the
  // configured flex range [minNote..maxNote], then map the ENTIRE flex travel
  // across that set. This way narrowing the range compresses the sweep (the
  // whole bend still spans the full available range) instead of truncating it
  // into dead zones at the ends. scaleDegreeToMidiPitch() is monotonically
  // ascending in buttonPos, so we can iterate upward and stop past maxNote.
  int candidates[40];
  int count = 0;
  int lastN = -1;
  for (int bp = 0; bp < 96; bp++) {
    int n = scaleDegreeToMidiPitch(bp) + keyTranspose;
    if (n < 0) continue;
    if (n > 127 || n > maxNote) break; // monotonic: nothing higher will fit
    if (n >= minNote && n != lastN) {
      lastN = n;
      if (count < 40) candidates[count++] = n;
      else break;
    }
  }

  int shifted;
  if (count > 0) {
    // Map flex 0..1 across the in-range note set (full travel = full range).
    int idx = (int)(flexBarBend * (count - 1) + 0.5f);
    if (idx < 0) idx = 0;
    if (idx >= count) idx = count - 1;
    shifted = candidates[idx];
  } else {
    // No scale note lands inside [minNote..maxNote]; fall back to the floor.
    shifted = constrain(minNote, 0, 127);
  }

  // Only queue if the resulting pitch differs from what's currently sounding
  // (or what's already queued).
  int target = pendingFlexNote >= 0 ? pendingFlexNote : lastFlexNote;
  if (shifted != target) {
    pendingFlexNote = shifted;
    pendingFlexNoteOff = false; // a new note pending cancels any pending off
  }
}

// Release the currently-sounding flex note immediately. Called when the
// index modal contact is released. NoteOff is NOT quantized -- waiting until
// the next grid tick would leave the note sounding noticeably past the
// contact release (up to flexQuantizeMs ms). Any pending pitch change in
// pendingFlexNote is also dropped, since releasing the contact obviously
// supersedes a queued pitch update.
void FlexNoteOff()
{
  if (lastFlexNote >= 0) {
    MIDI.sendNoteOff((byte)lastFlexNote, 0, MIDIchannel);
    lastFlexNote = -1;
  }
  pendingFlexNote = -1;
  pendingFlexNoteOff = false;
}

// Trigger a white-channel pulse on the flex bar display. Called when a flex
// NoteOn actually fires (at the quantize grid tick). Stacks additively with
// any in-flight pulse: residual W from the previous pulse is computed, added
// to the new pulse contribution, capped at 255.
void triggerFlexPulse()
{
  unsigned long now = millis();
  // Residual W from the previous pulse, if any.
  float residual = 0.0f;
  if (flexPulseBaseW > 0.0f) {
    unsigned long elapsed = now - flexPulseStartMs;
    if (elapsed < FLEX_PULSE_MS) {
      float k = (float)elapsed / (float)FLEX_PULSE_MS;
      residual = flexPulseBaseW * (1.0f - k);
    }
  }
  float total = residual + (float)FLEX_PULSE_W_PER_TRIGGER;
  if (total > 255.0f) total = 255.0f;
  flexPulseBaseW = total;
  flexPulseStartMs = now;
}

// Flex Note time quantization driver. 
// Called every loop iteration. When flex quantize is OFF,
// any pending pitch change fires immediately. Otherwise it's deferred to the
// next grid tick (advanced by flexQuantizeMs).
void FlexNoteFlush()
{
  unsigned long now = millis();

  // Helper to actually emit the pending flex note transition.
  // (NoteOff for the previous note first, then NoteOn for the new pitch.)
  // The flex contact release path (FlexNoteOff) sends NoteOff IMMEDIATELY
  // rather than queuing, so this only ever handles NoteOn transitions.
  if (flexQuantizeMode == FQ_OFF) {
    // No grid: fire pending pitch change right away.
    flexNextGridMs = 0; // keep grid disarmed so it re-aligns if mode changes
    if (pendingFlexNote >= 0) {
      if (lastFlexNote >= 0) {
        MIDI.sendNoteOff((byte)lastFlexNote, 0, MIDIchannel);
      }
      MIDI.sendNoteOn((byte)pendingFlexNote, 100, MIDIchannel);
      lastFlexNote = pendingFlexNote;
      pendingFlexNote = -1;
      triggerFlexPulse();
    }
    return;
  }

  // First call after boot (or after leaving OFF): align grid to now.
  if (flexNextGridMs == 0) {
    flexNextGridMs = now + flexQuantizeMs;
    return;
  }

  if ((long)(now - flexNextGridMs) < 0) return; // not yet

  // Advance the grid. If we somehow fell several ticks behind (rare), snap to
  // the next future tick rather than firing the queued event multiple times.
  while ((long)(now - flexNextGridMs) >= 0) {
    flexNextGridMs += flexQuantizeMs;
  }

  // Fire any pending pitch change. NoteOff for the previous note is sent
  // first (if any), then NoteOn for the new pitch.
  if (pendingFlexNote >= 0) {
    if (lastFlexNote >= 0) {
      MIDI.sendNoteOff((byte)lastFlexNote, 0, MIDIchannel);
    }
    MIDI.sendNoteOn((byte)pendingFlexNote, 100, MIDIchannel);
    lastFlexNote = pendingFlexNote;
    pendingFlexNote = -1;
    triggerFlexPulse(); // visual confirmation: pulse the W channel on lit LEDs
  }
}

// Finger-note quantization driver. Runs every loop iteration. When the next
// note-quantize grid tick is reached, fires any pending NoteOn events for all
// channels (chord mode -- all queued presses fire on the same tick) and
// advances the grid by the current note-quantize interval.
//
// If noteQuantizeMode is NQ_OFF, this is a no-op (no queueing happens in
// NoteOn()), and the grid is held disarmed so it re-aligns cleanly when the
// user switches into a quantized mode.
void FlushPendingNotes()
{
  if (noteQuantizeMode == NQ_OFF) {
    // Disarm the grid so it re-aligns on next mode change.
    noteQuantizeNextGridMs = 0;
    return;
  }

  unsigned long now = millis();
  unsigned long interval = noteQuantizeIntervalMs();
  if (interval == 0) return;

  // First call after entering quantize mode: align grid to now + interval.
  if (noteQuantizeNextGridMs == 0) {
    noteQuantizeNextGridMs = now + interval;
    return;
  }

  if ((long)(now - noteQuantizeNextGridMs) < 0) return; // not yet

  // Snap forward past any missed ticks (rare).
  while ((long)(now - noteQuantizeNextGridMs) >= 0) {
    noteQuantizeNextGridMs += interval;
  }

  // Fire all pending NoteOns as a chord at this tick.
  for (int ch = 0; ch < 16; ch++) {
    if (pendingNotePresent[ch]) {
      pendingNotePresent[ch] = false;
      NoteOnImmediate(ch);
    }
  }
}

// -----------------------------------------------------------------------------
// Note spread handling
// -----------------------------------------------------------------------------
// Note Spread: change the semitone spacing between the notes
// Examples of different spreads for note handling arrays for the 12 finger contact notes:
// {60,61,62,63,64,65,66,67,68,69,70,71}; // +1 Chromatic note
// {60,62,64,66,68,70,72,74,76,78,80,82}; // +2 semitone note
// {60,63,66,69,72,75,78,81,84,87,90,93}; // +3 semitone note
// {60,64,68,72,76,80,84,88,92,96,100,104}; // +4 semitone note

// NoteSpread() now lives in ScaleQuant.{h,cpp}.

// Returns true if any note button (mux channels 3..14) is currently held down.
// Used to gate the octave-up/down buttons so they don't fire accidentally while
// the player is in the middle of playing a chord. "Held" here means the most
// recent debounced state was LOW (= contact closed against ground via pull-up).
static bool anyNotePressed()
{
  for (int ch = 3; ch <= 14; ch++) {
    if (Buttons[ch] == LOW) return true;
  }
  return false;
}

// -----------------------------------------------------------------------------
// Note blocking in sub-modes
// -----------------------------------------------------------------------------
// All three modal contacts (octave-up, octave-down, pinky-palm) are blocked
// whenever a finger note is being played OR the index/flex modal is engaged.
// This prevents accidental mode changes during performance.
static bool modalActionsBlocked()
{
  return anyNotePressed() || indexHeld;
}

// LED display + tap-tempo helpers are declared in LedDisplay.h.


// -----------------------------------------------------------------------------
// Button & menu handling
// -----------------------------------------------------------------------------
//
// Debounce 4067 inputs and send MIDI notes or trigger index finger & pinky finger modal buttons accordingly
void debounceButton(int channel)
{ // Debounce signal input pin
  int buttonState = digitalReadFromMultiplexer(channel);

  if (buttonState != prevButtons[channel]) {
    lastDebounceTime[channel] = millis();
  }

  if ((millis() - lastDebounceTime[channel]) > debounceDelay) {
    if (buttonState != Buttons[channel]) {
      Buttons[channel] = buttonState;

      // While in tap-tempo mode, only the MENU channels are intercepted; all
      // other note channels fall through to the normal NoteOn branch below.
      // Menu channels (notes are suppressed for these while tap-tempo active):
      //   ch3  / ch6   = keyTranspose  +1 / -1   (range +/- 5 semitones)
      //   ch9  / ch12  = rootTranspose +1 / -1   (range +/- 5 semitones)
      //   ch5  / ch8   = tempo         +1 / -1 BPM   (shows tempo display)
      //   ch4          = toggle CC1/CC11 axis swap  (shows swap display)
      // tapTempoTransposeMode tracks which menu was last used to colour LED 3:
      //   1 = keyTranspose (white blink), 2 = rootTranspose (blue blink),
      //   3 = tempo nudge (tempo display takes over the strip),
      //   4 = CC swap (swap display takes over the strip).
      if (tapTempoActive && (buttonState == LOW) &&
          (channel == 3 || channel == 4 || channel == 5 || channel == 6 ||
           channel == 8 || channel == 9 || channel == 12)) {
        if (flexRangeEditActive &&
            (channel == 3 || channel == 6 || channel == 9 || channel == 12)) {
          // Flex-range editor: ch3/ch6 = maxNote +/-, ch9/ch12 = minNote +/-.
          // Endpoints are kept ordered (min < max) and within absolute bounds.
          if (channel == 3) {
            if (maxNote < FLEX_NOTE_MAX) maxNote++;
            flexRangeEditTarget = 2;
            DBG_PRINT("Flex maxNote +1 -> "); DBG_PRINTLN(maxNote);
          } else if (channel == 6) {
            if (maxNote > minNote + 1) maxNote--;
            flexRangeEditTarget = 2;
            DBG_PRINT("Flex maxNote -1 -> "); DBG_PRINTLN(maxNote);
          } else if (channel == 9) {
            if (minNote < maxNote - 1) minNote++;
            flexRangeEditTarget = 1;
            DBG_PRINT("Flex minNote +1 -> "); DBG_PRINTLN(minNote);
          } else if (channel == 12) {
            if (minNote > FLEX_NOTE_MIN) minNote--;
            flexRangeEditTarget = 1;
            DBG_PRINT("Flex minNote -1 -> "); DBG_PRINTLN(minNote);
          }
          lastFlexRangePressMs = millis();
          lastTapMs = millis();
        } else if (channel == 3) {
          if (keyTranspose < KEY_TRANSPOSE_MAX) {
            keyTranspose++;
            tapTempoTransposeMode = 1;
            DBG_PRINT("Key transpose +1 -> ");
            DBG_PRINTLN(keyTranspose);
          }
        } else if (channel == 6) {
          if (keyTranspose > KEY_TRANSPOSE_MIN) {
            keyTranspose--;
            tapTempoTransposeMode = 1;
            DBG_PRINT("Key transpose -1 -> ");
            DBG_PRINTLN(keyTranspose);
          }
        } else if (channel == 9) {
          if (rootTranspose < ROOT_TRANSPOSE_MAX) {
            rootTranspose++;
            tapTempoTransposeMode = 2;
            DBG_PRINT("Root transpose +1 -> ");
            DBG_PRINTLN(rootTranspose);
          }
        } else if (channel == 12) {
          if (rootTranspose > ROOT_TRANSPOSE_MIN) {
            rootTranspose--;
            tapTempoTransposeMode = 2;
            DBG_PRINT("Root transpose -1 -> ");
            DBG_PRINTLN(rootTranspose);
          }
        } else if (channel == 5) {
          // ch5 has two gestures: quick tap = tempo +1, 1 s hold = toggle the
          // flex-range editor. Defer the tempo nudge to release so a hold does
          // not also nudge. Just record the press edge here.
          ch5PressedMs = millis();
          ch5HoldFired = false;
        } else if (channel == 8) {
          // Tempo - 1 (clamped to TEMPO_MIN). Show the tempo display.
          if (tempo > TEMPO_MIN) {
            setTempo(tempo - 1);
            tapTempoTransposeMode = 3;
            DBG_PRINT("Tempo -1 -> ");
            DBG_PRINTLN(tempo);
          }
        } else if (channel == 4) {
          // Toggle the CC1/CC11 axis swap. Show the swap display.
          ccSwapped = !ccSwapped;
          unsigned int tmp = CCAccelX;
          CCAccelX = CCAccelY;
          CCAccelY = tmp;
          tapTempoTransposeMode = 4;
          DBG_PRINT("CC swap -> ");
          DBG_PRINTLN(ccSwapped ? "SWAPPED (Y=CC1, X=CC11)" : "default (X=CC1, Y=CC11)");
        }
        // Refresh the inactivity timer so the menu stays open while in use.
        lastTapMs = millis();
      }
      // ch5 release while in tap-tempo: if it was a quick tap (not a 1 s hold),
      // apply the deferred tempo +1. A hold is consumed by the editor toggle.
      else if (tapTempoActive && (buttonState == HIGH) && (channel == 5)) {
        if (!ch5HoldFired) {
          if (tempo < TEMPO_MAX) {
            setTempo(tempo + 1);
            tapTempoTransposeMode = 3;
            DBG_PRINT("Tempo +1 -> ");
            DBG_PRINTLN(tempo);
          }
          lastTapMs = millis();
        }
        ch5HoldFired = false;
      }
      // Preset-browser active: intercept the control channels.
      //   ch11 -> cycle brightness level (off / 10 / 30 / 60 / 120 / 180 / 255)
      //   ch9  -> cycle flex quantization mode (OFF / 1/8 / 1/16 / 1/32)
      //   ch12 -> cycle note quantization mode (OFF / 1/8 / 1/16 / 1/32)
      //   ch8  -> toggle tempo-tracking pitch bend (on / off)
      //   ch3 / ch6 -> handled time-based below (preset scroll); swallow here so
      //                they never play a note.
      // Other note channels (4,5,7,10,13,14) fall through and play normally
      // so the auditioned settings can be heard. ch2 (flex modal) load is
      // handled in the channel-2 block below.
      else if (presetBrowserActive && (buttonState == LOW) && (channel == 8)) {
        tempoBendEnabled = !tempoBendEnabled;
        lastSentBendTempo = -1; // force a re-evaluation on next tick
        presetSubView = 3;      // show the tempo-bend status view
        DBG_PRINT("Tempo bend -> ");
        DBG_PRINTLN(tempoBendEnabled ? "ENABLED" : "disabled");
      }
      else if (presetBrowserActive && (buttonState == LOW) && (channel == 11)) {
        brightnessLevel = (brightnessLevel + 1) % BRIGHTNESS_LEVEL_COUNT;
        applyStripBrightness(brightnessLevel);
#ifdef NEOPIXEL_ENABLED
																
        ledForceRepush = true; // brightness change alone doesn't alter curPix
#endif
        presetSubView = 2; // show the brightness rainbow preview
        DBG_PRINT("Brightness level -> ");
        DBG_PRINTLN(BRIGHTNESS_LEVELS[brightnessLevel]);
      }
      else if (presetBrowserActive && (buttonState == LOW) && (channel == 9)) {
        flexQuantizeMode = (flexQuantizeMode + 1) % FQ_MODE_COUNT;
        setTempo(tempo); // re-derive flex grid for the new subdivision
        flexNextGridMs = 0; // re-align flex grid
        presetSubView = 1;  // show the quantize display
        DBG_PRINT("Flex quantize mode -> ");
        DBG_PRINTLN(flexQuantizeMode == FQ_OFF ? "OFF"
                       : flexQuantizeMode == FQ_1_8 ? "1/8"
                       : flexQuantizeMode == FQ_1_16 ? "1/16"
                                                     : "1/32");
      }
      else if (presetBrowserActive && (buttonState == LOW) && (channel == 12)) {
        noteQuantizeMode = (noteQuantizeMode + 1) % NQ_MODE_COUNT;
        noteQuantizeNextGridMs = 0; // re-align grid
        presetSubView = 1;  // show the quantize display
        DBG_PRINT("Note quantize mode -> ");
        DBG_PRINTLN(noteQuantizeMode == NQ_OFF ? "OFF"
                       : noteQuantizeMode == NQ_1_8 ? "1/8"
                       : noteQuantizeMode == NQ_1_16 ? "1/16"
                                                     : "1/32");
      }
      else if (presetBrowserActive && (channel == 3 || channel == 6)) {
        // Preset scroll handled time-based below; swallow press/release here.
      }
      // Scale-edit latch active: intercept ONLY ch3 (scale up) and ch6 (scale
      // down). Each press steps the scale once with wrap-around (0..19). All
      // other note channels fall through to the normal NoteOn branch below so
      // the scale can be auditioned live before exiting the latch.
      else if (scaleEditActive && (buttonState == LOW) && (channel == 3)) {
        Scale = (Scale + 1) % 20;
        lastScaleEditPressMs = millis();
        DBG_PRINT("Scale up -> ");
        DBG_PRINTLN(Scale);
#ifdef NEOPIXEL_ENABLED
        TriggerScaleDisplay();
#endif
      }
      else if (scaleEditActive && (buttonState == LOW) && (channel == 6)) {
        Scale = (Scale + 19) % 20; // +19 mod 20 = decrement with wrap
        lastScaleEditPressMs = millis();
        DBG_PRINT("Scale down -> ");
        DBG_PRINTLN(Scale);
#ifdef NEOPIXEL_ENABLED
        TriggerScaleDisplay();
#endif
      }
      // ch3/ch6 release while scale-edit active: swallow (no NoteOff needed,
      // no NoteOn was ever sent for them).
      else if (scaleEditActive && (buttonState == HIGH) && (channel == 3 || channel == 6)) {
        // Intentionally do nothing.
      }
      // Trigger Note On when signal pin input is low and only on 4067 channels 3-14
      // (normal play -- only when tap tempo is NOT active and flex modal is NOT held).
      else if ((buttonState == LOW) && (channel > 2) && (channel < 15) && !indexHeld) {
        NoteOn(channel);
        noteOffFlag[channel] = false; // Reset Note Off flag when Note On is triggered
      }
      // Set Note Off flag when button is released from 4067 channels 3-14.
      // (Safe to set even if no NoteOn fired -- NoteOff short-circuits when
      // there's no active note on that channel.)
      else if ((channel > 2) && (channel < 15)) {
        noteOffFlag[channel] = true;
      }
      // Pinky palm contact: a quick tap toggles NoteSpread (cycle 1->2->3->4),
      // but holding for >= 1 s instead cycles the Scale to the next one (Scale
      // cycle fires at the 1 s mark; the on-release Spread toggle is then
      // suppressed). The held-time check itself happens OUTSIDE this state-
      // change block (see further below) since it's time-based, not edge-based.
      else if ((buttonState == LOW) && (channel == 15)) {
        // On press: record timestamp; defer the action decision until release
        // or until the 1 s hold mark is reached.
        pinkyPressedMs = millis();
        pinkyScaleFired = false;
      } else if ((buttonState == HIGH) && (channel == 15)) {
        // On release: if the Scale didn't fire during the hold, treat this as
        // a quick tap and cycle Spread.
        if (!pinkyScaleFired) {
          Spread = (Spread % 4) + 1;
          NoteSpread(RootNote, Spread, RootNoteOffset);
#ifdef NEOPIXEL_ENABLED
          TriggerSpreadDisplay();
#endif
        }
      }
      // Top left of middle finger contact: octave DOWN.
      // Fires on RELEASE. Also: holding for >= 1 s cycles the note-quantize
      // mode (off / 1-16 / 1-32) and suppresses the octave change.
      else if ((buttonState == LOW) && (channel == 1)) {
        // Press: record timestamp; defer action to release.
        octaveDownPressedMs = millis();
        octaveDownHoldFired = false;
      } else if ((buttonState == HIGH) && (channel == 1)) {
        if (octaveDownHoldFired) {
          // Hold already cycled the quantize mode; do not apply octave change.
          octaveDownHoldFired = false;
        } else {
          // Quick tap: apply octave down if no notes and no index/flex held.
          if (!modalActionsBlocked()) {
            if (octave > 0) octave--;
            DBG_PRINT("Octave Down -> ");
            DBG_PRINT(octave);
            DBG_PRINT(" (");
            DBG_PRINT(OCTAVE_OFFSETS[octave]);
            DBG_PRINTLN(" semitones)");
#ifdef NEOPIXEL_ENABLED
            TriggerOctaveDisplay();
#endif
          } else {
            DBG_PRINTLN("Octave Down ignored (notes/index held)");
          }
        }
      }
      // Top right of index finger contact: octave UP.
      // Fires on RELEASE. Also: holding for >= 1 s enters tap-tempo mode
      // (and suppresses the octave change). While in tap-tempo mode, the press
      // is consumed as a tap rather than an octave action.
      else if ((buttonState == LOW) && (channel == 0)) {
        if (tapTempoActive && !tapTempoConfirming) {
          // We're capturing taps -- this press counts as a tap, not an octave.
          recordTapTempoPress();
        } else {
          // Normal flow: record press time, defer action to release.
          octaveUpPressedMs = millis();
          octaveUpHoldFired = false;
        }
      } else if ((buttonState == HIGH) && (channel == 0)) {
        if (tapTempoActive) {
          // Tap-tempo capture absorbs the release silently.
          // (recordTapTempoPress() was called on the press edge.)
        } else if (octaveUpHoldFired) {
          // Hold gesture already entered tap-tempo on the press side;
          // do not apply an octave change on release.
          octaveUpHoldFired = false;
        } else {
          // Quick tap: apply octave up if no notes are held.
          if (!anyNotePressed()) {
            if (octave < OCTAVE_MAX_INDEX) octave++;
            DBG_PRINT("Octave Up -> ");
            DBG_PRINT(octave);
            DBG_PRINT(" (");
            DBG_PRINT(OCTAVE_OFFSETS[octave]);
            DBG_PRINTLN(" semitones)");
#ifdef NEOPIXEL_ENABLED
            TriggerOctaveDisplay();
#endif
          } else {
            DBG_PRINTLN("Octave Up ignored (notes held)");
          }
        }
      }
      // Side of index finger contact: momentary modal enabling flex-sensor
      // note output and AccelY MIDI CC while held. BUT while the preset browser
      // is active, touching it instead LOADS the selected preset and exits the
      // browser (it does NOT engage flex mode).
      else if ((buttonState == LOW) && (channel == 2)) {
        if (presetBrowserActive) {
          bool ok = loadPatch(selectedPreset);
          (void)ok; // used only by debug print, which may be compiled out
          presetBrowserActive = false;
#ifdef NEOPIXEL_ENABLED
          if (ok) TriggerPatchConfirm(true); // green->cyan load confirmation
#endif
          DBG_PRINT("Preset browser EXIT via load, slot ");
          DBG_PRINT(selectedPreset);
          DBG_PRINTLN(ok ? " (loaded)" : " (empty - nothing loaded)");
        } else {
          indexHeld = true;
          lastFlexNote = -1; // ensure first flex sample triggers a NoteOn cleanly
          DBG_PRINTLN("Index modal pressed (flex+AccelY active)");
        }
      } else if ((buttonState == HIGH) && (channel == 2)) {
        // Only tear down flex state if flex mode was actually engaged.
        if (indexHeld) {
          indexHeld = false;
          FlexNoteOff(); // release any currently sounding flex note
          // Clear flex bar / pulse state so re-engagement starts clean.
          flexPulseBaseW = 0.0f;
          flexPulseStartMs = 0;
          flexBarBend = 0.0f;
          DBG_PRINTLN("Index modal released");
        }
      }
    }
  }

  // Pinky palm (channel 15): held >= 1 s.
  //   - While preset browser active: SAVE current settings to selected slot.
  //   - Otherwise: TOGGLE the scale-edit latch.
  // Time-based; pinkyScaleFired guards against re-firing within one hold.
  if (channel == 15 && Buttons[15] == LOW && !pinkyScaleFired) {
    if ((millis() - pinkyPressedMs) >= PINKY_SCALE_HOLD_MS) {
      pinkyScaleFired = true;
      if (presetBrowserActive) {
        bool ok = savePatch(selectedPreset);
        (void)ok; // used only by debug print, which may be compiled out
        presetBrowserActive = false;
#ifdef NEOPIXEL_ENABLED
        if (ok) TriggerPatchConfirm(false); // orange->cyan save confirmation
#endif
        DBG_PRINT("Preset SAVE to slot ");
        DBG_PRINT(selectedPreset);
        DBG_PRINT(ok ? " (ok)" : " (FAILED)");
        DBG_PRINTLN(" - browser EXIT");
      } else {
        scaleEditActive = !scaleEditActive;
        if (scaleEditActive) lastScaleEditPressMs = millis(); // start the auto-exit timer
        DBG_PRINT("Scale-edit latch -> ");
        DBG_PRINTLN(scaleEditActive ? "ON" : "OFF");
#ifdef NEOPIXEL_ENABLED
        TriggerScaleDisplay();
#endif
      }
    }
  }
  // Octave-up held >= 1 s -> enter tap-tempo mode. This is the gateway to the
  // transpose menu (ch3/6/9/12) and the new tempo-nudge controls (ch2/5).
  // Even when locked to an incoming MIDI clock the menu is still entered so
  // the transpose / nudge controls remain reachable; the actual TAPPING
  // (rebuilding internal tempo from 8 taps) is disabled while clocked --
  // recordTapTempoPress() checks externalClockActive and refuses to update.
  if (channel == 0 && Buttons[0] == LOW && !octaveUpHoldFired) {
    if ((millis() - octaveUpPressedMs) >= OCTAVE_UP_HOLD_MS) {
      octaveUpHoldFired = true;
      if (!tapTempoActive) {
        enterTapTempoMode();
      }
    }
  }

  // ch5 held >= 1 s while in tap-tempo -> toggle the flex-range editor.
  if (tapTempoActive && channel == 5 && Buttons[5] == LOW && !ch5HoldFired) {
    if ((millis() - ch5PressedMs) >= CH5_HOLD_MS) {
      ch5HoldFired = true; // suppresses the tempo nudge on release
      flexRangeEditActive = !flexRangeEditActive;
      if (flexRangeEditActive) {
        flexRangeEditTarget = 2;           // default to showing max first
        lastFlexRangePressMs = millis();
      } else if (!indexHeld) {
        FlexNoteOff();                      // release audition note on exit
      }
      lastTapMs = millis();
      DBG_PRINT("Flex-range editor -> ");
      DBG_PRINTLN(flexRangeEditActive ? "ON" : "OFF");
    }
  }

  // Octave-down (channel 1): held >= 1 s -> toggle the preset-browser mode.
  // Entering arms browse state; exiting via this same gesture leaves WITHOUT
  // loading or saving (any newly chosen quantization mode is retained).
  if (channel == 1 && Buttons[1] == LOW && !octaveDownHoldFired) {
    if ((millis() - octaveDownPressedMs) >= OCTAVE_DOWN_HOLD_MS) {
      octaveDownHoldFired = true;
      if (!presetBrowserActive) {
        presetBrowserActive = true;
        presetSubView = 0; // show the preset band immediately on entry
        // Disable flex-sensor notes while browsing.
        if (indexHeld) {
          indexHeld = false;
          FlexNoteOff();
          flexPulseBaseW = 0.0f;
          flexPulseStartMs = 0;
          flexBarBend = 0.0f;
        }
        DBG_PRINT("Preset browser ENTERED, slot ");
        DBG_PRINTLN(selectedPreset);
      } else {
        presetBrowserActive = false;
        DBG_PRINTLN("Preset browser EXIT (no load/save)");
      }
    }
  }

  // Check if it's time to send NoteOff
  if (noteOffFlag[channel] && millis() > (lastDebounceTime[channel] + noteOffDelay)) {
    noteOffFlag[channel] = false; // Reset Note Off flag
    NoteOff(channel);
  }

  prevButtons[channel] = buttonState;
}


// IMU Double Tap Interrupt Setup
void setupDoubleTapInterrupt()
{
  //  uint8_t error = 0;        // error: unused variable
  //  uint8_t dataToWrite = 0;  // error: unused variable

  // Double Tap Config
  myIMU.writeRegister(LSM6DS3_ACC_GYRO_CTRL1_XL, 0x60);
  myIMU.writeRegister(LSM6DS3_ACC_GYRO_TAP_CFG1, 0x8E); // INTERRUPTS_ENABLE, SLOPE_FDS
  myIMU.writeRegister(LSM6DS3_ACC_GYRO_TAP_THS_6D, 0x8C);
  myIMU.writeRegister(LSM6DS3_ACC_GYRO_INT_DUR2, 0x7F);
  myIMU.writeRegister(LSM6DS3_ACC_GYRO_WAKE_UP_THS, 0x80);
  myIMU.writeRegister(LSM6DS3_ACC_GYRO_MD1_CFG, 0x08);
}

// Onboard XIAO Sense RGB LED control
void setLED_REDGB(bool red, bool green, bool blue)
{
  if (!blue) {
    digitalWrite(LED_BLUE, HIGH);
  } else {
    digitalWrite(LED_BLUE, LOW);
  }
  if (!green) {
    digitalWrite(LED_GREEN, HIGH);
  } else {
    digitalWrite(LED_GREEN, LOW);
  }
  if (!red) {
    digitalWrite(LED_RED, HIGH);
  } else {
    digitalWrite(LED_RED, LOW);
  }
}

// IMU Double Tap Interrupt handler. Kept minimal -- no blocking calls, no
// Serial, no analogRead. Sets a flag that loop() picks up and services.
void int1ISR()
{
  DoubleTapState = !DoubleTapState;
  setLED_REDGB(false, DoubleTapState, false); // set green only
  doubleTapBatteryCheckPending = true;
  // No Serial here -- this is an ISR; blocking USB-CDC I/O is unsafe.
}



void setup()
{

  //  while (!Serial)
  ;
  // Call .begin() to configure the IMUs
  if (myIMU.begin() != 0) {
    BOOT_PRINTLN("Device error");
  } else {
    BOOT_PRINTLN("Device OK!");
  }

  myIMU.begin();
  uint8_t dataToWrite = 0; // Temporary variable

  // Setup the accelerometer******************************
  dataToWrite = 0; // Start Fresh!
  dataToWrite |= LSM6DS3_ACC_GYRO_BW_XL_100Hz;
  dataToWrite |= LSM6DS3_ACC_GYRO_FS_XL_2g;
  dataToWrite |= LSM6DS3_ACC_GYRO_ODR_XL_104Hz;

  // Now, write the patched together data
  errorsAndWarnings += myIMU.writeRegister(LSM6DS3_ACC_GYRO_CTRL1_XL, dataToWrite);

  // Set the ODR bit
  errorsAndWarnings += myIMU.readRegister(&dataToWrite, LSM6DS3_ACC_GYRO_CTRL4_C);
  dataToWrite &= ~((uint8_t)LSM6DS3_ACC_GYRO_BW_SCAL_ODR_ENABLED);

  Serial.begin(115200);
  //  while (!Serial)
  //    delay(10); // for nrf52840 with native usb

  BOOT_PRINTLN("Adafruit Bluefruit52 MIDI over Bluetooth LE Example");

  // Initialize the internal-flash filesystem used for patch save/recall.
  if (!InternalFS.begin()) {
    BOOT_PRINTLN("InternalFS begin failed -- patch save/load unavailable");
  }

#if FACTORY_RESET
  // Wipe everything in internal flash: all patches AND the BLE bond store
  // (Bluefruit keeps bonds as files under InternalFS). Done BEFORE
  // Bluefruit.begin() so the BLE stack comes up with a clean, unbonded state.
  Serial.println("; FACTORY RESET: formatting internal flash (patches + BLE bonds)...");
  InternalFS.format();
  InternalFS.begin();
  Serial.println("; FACTORY RESET complete. Set FACTORY_RESET back to 0 and reflash.");
  Serial.println("; Also 'forget' the glove on your host's Bluetooth list, then re-pair.");
#endif

  // Announce the serial patch console (works over USB regardless of BLE state).
  Serial.println("; patch console ready -- type HELP for commands");

  // Config the peripheral connection with maximum bandwidth
  Bluefruit.configPrphBandwidth(BANDWIDTH_MAX);

  Bluefruit.begin();
  Bluefruit.setName("Sneaky Gestures MIDI");
  Bluefruit.setTxPower(8);

  // Setup the on board blue LED to be enabled on CONNECT
  Bluefruit.autoConnLed(true);

  // Configure and Start BLE Device Information Service
  bledis.setManufacturer("Adafruit Industries");
  bledis.setModel("Bluefruit Feather52");
  bledis.begin();

  // Initialize MIDI, and listen to all MIDI channels, will also call blemidi service's begin()
  MIDI.begin(MIDI_CHANNEL_OMNI);

  // Disable MIDI Thru entirely -- the glove never re-transmits incoming
  // messages. This is especially important for clock messages: we receive
  // clock to drive our internal tempo, but never want to forward it.
  MIDI.turnThruOff();

  // Attach the handleNoteOn function to the MIDI Library. It will
  // be called whenever the Bluefruit receives MIDI Note On messages.
  MIDI.setHandleNoteOn(handleNoteOn);

  // Do the same for MIDI Note Off messages.
  MIDI.setHandleNoteOff(handleNoteOff);

  // MIDI real-time clock (0xF8) at 24 PPQN. When received, overrides the
  // internal tempo and disables tap tempo.
  MIDI.setHandleClock(handleClock);

  // MIDI Program Change: load preset 0..62 (PC number) and briefly show its bar.
  MIDI.setHandleProgramChange(handleProgramChange);

  // Set up and start advertising
  startAdv();

  // Start MIDI read loop
  Scheduler.startLoop(midiRead);

  // Initialize 4067 Multiplexer
  pinMode(inputPin, INPUT_PULLUP); // Initialize 4067 signal pin return as input with 3.3V internal pullup
  // pinMode(muxEnablePin, OUTPUT);    // Initialize 4067 Enable pin as Output
  // digitalWrite(muxEnablePin, HIGH); // Turn on 4067 Enable pin permanently

  // Seed the button debounce state to the released (HIGH) baseline. These
  // arrays live in BSS and would otherwise start at 0 (== LOW == "pressed"),
  // so the very first scan after a BLE connect would see every pad as held
  // since boot and fire every hold-gesture at once (tap-tempo, preset browser,
  // flex-range editor, save). Starting HIGH means a pad only registers once a
  // real release->press edge occurs.
  for (int i = 0; i < numChannels; i++) {
    Buttons[i]          = HIGH;
    prevButtons[i]      = HIGH;
    lastDebounceTime[i] = millis();
  }

  // Initiate note array starting with the root note and how many semitones until the next note, aka Spread
  NoteSpread(RootNote, Spread, RootNoteOffset);

  // Initialize global tempo (and derive flexQuantizeMs from it).
  setTempo(tempo);

  // Setup IMU Double Tap Interrupt
  setupDoubleTapInterrupt();
  pinMode(int1Pin, INPUT);
  attachInterrupt(digitalPinToInterrupt(int1Pin), int1ISR, RISING);


#ifdef NEOPIXEL_ENABLED // NEOPIXEL animations

  // Initialize NeoPixels (Adafruit_NeoPixel uses PWM DMA on the nRF52840 BSP).
  strip.begin();
  applyStripBrightness(brightnessLevel);
  strip.show(); // turn OFF all pixels

  // Initialize battery monitor and arm the 2 s non-blocking boot indicator.
  // The strip will display capacity bar / charging state for BATTERY_DISPLAY_MS,
  // after which UpdateFingerLeds() will resume normal finger animations.
  xiao.begin();
  TriggerBatteryDisplay();
#endif           // NEOPIXEL_ENABLED for NEOPIXEL Animations
}

// Preset-browser scroll driver. Called once per loop() AFTER the full mux scan
// so Buttons[3]/Buttons[6] reflect current debounced state. ch3 = next preset,
// ch6 = previous preset, both wrapping 0..62. Holding either auto-repeats after
// PRESET_SCROLL_REPEAT_DELAY_MS. No patch is loaded here -- only the selected
// indicator changes. The pressed-timestamp resets when the channel is HIGH so
// re-pressing always starts a fresh initial-delay window.
void UpdatePresetScroll()
{
  if (!presetBrowserActive) return;
  unsigned long now = millis();

  // Any ch3/ch6 activity switches the strip to the preset band sub-view.
  if (Buttons[3] == LOW || Buttons[6] == LOW) {
    presetSubView = 0;
  }

  if (Buttons[3] == HIGH) {
    presetScrollPressedMs[3] = 0;
    presetScrollLastStepMs[3] = 0;
  }
  if (Buttons[6] == HIGH) {
    presetScrollPressedMs[6] = 0;
    presetScrollLastStepMs[6] = 0;
  }

  if (Buttons[3] == LOW) {
    if (presetScrollPressedMs[3] == 0) {
      presetScrollPressedMs[3] = now;
      presetScrollLastStepMs[3] = now;
      selectedPreset = (selectedPreset + 1) % PRESET_COUNT;
      DBG_PRINT("Preset -> ");
      DBG_PRINTLN(selectedPreset);
    } else if ((now - presetScrollPressedMs[3]) >= PRESET_SCROLL_REPEAT_DELAY_MS &&
               (now - presetScrollLastStepMs[3]) >= PRESET_SCROLL_REPEAT_INTERVAL_MS) {
      selectedPreset = (selectedPreset + 1) % PRESET_COUNT;
      presetScrollLastStepMs[3] = now;
      DBG_PRINT("Preset -> ");
      DBG_PRINTLN(selectedPreset);
    }
  }

  if (Buttons[6] == LOW) {
    if (presetScrollPressedMs[6] == 0) {
      presetScrollPressedMs[6] = now;
      presetScrollLastStepMs[6] = now;
      selectedPreset = (selectedPreset + PRESET_COUNT - 1) % PRESET_COUNT;
      DBG_PRINT("Preset -> ");
      DBG_PRINTLN(selectedPreset);
    } else if ((now - presetScrollPressedMs[6]) >= PRESET_SCROLL_REPEAT_DELAY_MS &&
               (now - presetScrollLastStepMs[6]) >= PRESET_SCROLL_REPEAT_INTERVAL_MS) {
      selectedPreset = (selectedPreset + PRESET_COUNT - 1) % PRESET_COUNT;
      presetScrollLastStepMs[6] = now;
      DBG_PRINT("Preset -> ");
      DBG_PRINTLN(selectedPreset);
    }
  }
}

void loop()
{

#ifdef NEOPIXEL_ENABLED
  // Drive the LED strip BEFORE the BLE connection gates below, so the battery
  // boot indicator and other animations render even when no MIDI host is
  // connected. The function self-throttles internally (~60 Hz).
  UpdateFingerLeds();
#endif

  // Handle an incoming MIDI Program Change (flagged by handleProgramChange in
  // the BLE task). Load the patch and briefly show its preset bar on the strip.
  if (pendingPcLoad) {
    int slot = pendingPcSlot;
    pendingPcLoad = false;
    bool ok = loadPatch(slot);
    (void)ok; // used only by debug print, which may be compiled out
    selectedPreset = slot; // keep the browser selection in sync
    pcPresetDisplaySlot = slot;
    pcPresetDisplayStartMs = millis();
    pcPresetDisplayActive = true;
    DBG_PRINT("PC received -> preset ");
    DBG_PRINT(slot);
    DBG_PRINTLN(ok ? " (loaded)" : " (empty - nothing loaded)");
  }

  // Service the double-tap battery check. The ISR only sets a flag; the
  // analogRead and display work happens here in the main loop context. Placed
  // ABOVE the BLE-connection gates so a battery check works even when no MIDI
  // host is connected.
  if (doubleTapBatteryCheckPending) {
    doubleTapBatteryCheckPending = false;
#ifdef NEOPIXEL_ENABLED
    TriggerBatteryDisplay(); // re-reads voltage and starts the 2 s display
#endif
  }

  // Service the serial patch console (HELP / LIST / DUMP / LOAD / FORMAT).
  // Placed ABOVE the BLE-connection gates so backup and restore work over USB
  // even when no MIDI host is connected.
  serviceSerialConsole();

  // Don't continue if we aren't connected.
  if (!Bluefruit.connected()) {
    return;
  }
  // Don't continue if the connected device isn't ready to receive messages.
  if (!blemidi.notifyEnabled()) {
    return;
  }

  // Start cycling through the 4067 channels to read the thumb contact touching the finger pads


  for (int channel = firstChannel; channel < numChannels; ++channel) {
    // Debounce the incoming input from the thumb connector that's connected to ground and pulling the input low
    debounceButton(channel);
  }

  // Preset-browser scroll (ch3/ch6 hold-repeat). Runs after the full scan so
  // Buttons[3]/Buttons[6] are current. No-op unless the browser is active.
  UpdatePresetScroll();

  // Check analog values
  int flexVal = analogRead(indexFLEX);
  int aftertouchVal = analogRead(thumbFSR);

  // send new mod value if it has changed
  if (lastFlexVal != flexVal) {

    lastFlexVal = flexVal;
  }

  // send new pitch value if it has changed
  if (lastFsrVal != aftertouchVal) {

    lastFsrVal = aftertouchVal;
  }


  // Index modal contact (channel 2) held: continuously map indexFLEX to MIDI
  // notes within [minNote..maxNote]. Also active while the flex-range editor is
  // open (tap-tempo sub-menu) so the range can be auditioned by bending without
  // holding the index modal. The function rate-limits itself.
  if (indexHeld || flexRangeEditActive) {
    FlexNoteUpdate();
  }

  // Drive the flex-note quantization grid. Runs unconditionally so that a
  // NoteOff queued at button release still fires at the next grid tick.
  FlexNoteFlush();

  // Drive the finger-note (NoteOn) quantization grid. No-op when mode is OFF.
  FlushPendingNotes();

  // Tap-tempo state machine (timeout handling, confirmation expiry).
  updateTapTempo();

  // Scale-edit auto-exit: leave the latch if no scale-select press for 1 s.
  if (scaleEditActive && (millis() - lastScaleEditPressMs) >= SCALE_EDIT_TIMEOUT_MS) {
    scaleEditActive = false;
    DBG_PRINTLN("Scale-edit auto-exit (timeout)");
  }

  // Flex-range editor auto-exit: leave if no edit press for FLEX_RANGE_TIMEOUT_MS.
  if (flexRangeEditActive && (millis() - lastFlexRangePressMs) >= FLEX_RANGE_TIMEOUT_MS) {
    flexRangeEditActive = false;
    if (!indexHeld) FlexNoteOff(); // release any audition note
    DBG_PRINTLN("Flex-range editor auto-exit (timeout)");
  }
  // If tap-tempo mode ends for any reason, the editor closes with it.
  if (!tapTempoActive && flexRangeEditActive) {
    flexRangeEditActive = false;
    if (!indexHeld) FlexNoteOff();
  }

  // Incoming MIDI clock state machine. Sets/clears externalClockActive based
  // on whether clock pulses are arriving; while locked, updates tempo from
  // the EMA-derived BPM whenever it has changed by >= 1 BPM.
  updateMidiClockState();

  // Tempo-tracking pitch bend: once/sec, compare incoming tempo to the saved
  // project tempo and emit a retune pitch bend if they differ (when enabled).
  UpdateTempoBend();

  // Send accelerometer MIDI CC's every X ms
  if (millis() - lastExecutionTimeAccel >= intervalAccel) {
    accelRead(); // Send accelerometer MIDI CC's

    // Update the last execution time
    lastExecutionTimeAccel = millis();
  }

  // Send channel aftertouch from the thumb FSR for any note held >= 50 ms
  UpdateAftertouch();

  // Re-send any dropped NoteOff (BLE stuck-note prevention).
  UpdateNoteOffRetries();

  // Idle All-Notes-Off safeguard (once/sec when nothing is sounding).
  UpdateIdlePanic();
}
