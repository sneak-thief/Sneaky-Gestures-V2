/*****************************************************************************/
// Sneaky Gestures V2: MIDI BLE Gestural Glove
// https://github.com/sneak-thief/Sneaky-Gestures-V2
// LED display handler
// ---------------------------------------------------------------------------

#include "LedDisplay.h"

#include <Arduino.h>
#include <math.h>
#include <light_CD74HC4067.h>
#include "LSM6DS3.h"
#include "DebugSerial.h"

#ifndef LED_PIN
#define LED_PIN 5
#endif

// Forward declaration (defined later in this file).
static inline void clampDataPinLow();

// ---------------------------------------------------------------------------
// External state owned by main.cpp that the LED module reads. These mirror the
// definitions in main.cpp; keep them in sync if the originals change.
// ---------------------------------------------------------------------------

// Hardware objects
extern Adafruit_NeoPixel strip;

// Note layout / musical state
extern unsigned int Keys[];
extern unsigned int Spread;
extern unsigned int Scale;
extern int octave;
extern const int OCTAVE_DEFAULT;
extern int noteQuantizeMode;
extern int flexQuantizeMode;
extern int keyTranspose;
extern int rootTranspose;
extern int tempo;

// Note-quantize / flex-quantize enum values (defined in main.cpp)
// (Used by renderQuantizeDisplay; declared there as plain ints.)

// Brightness
extern int brightnessLevel;
extern const uint8_t BRIGHTNESS_LEVELS[7];
extern const int BRIGHTNESS_LEVEL_COUNT;
extern volatile bool ledForceRepush;

// Preset browser
extern bool presetBrowserActive;
extern bool scaleEditActive;
extern int  selectedPreset;
extern int  presetSubView;
extern const int PRESET_COUNT;
extern bool tempoBendEnabled;

// Modal indicator flags + timers (700 ms etc.)
extern bool octaveDisplayActive;   extern unsigned long octaveDisplayStartMs;
extern bool spreadDisplayActive;   extern unsigned long spreadDisplayStartMs;
extern bool scaleDisplayActive;    extern unsigned long scaleDisplayStartMs;
extern bool quantizeDisplayActive; extern unsigned long quantizeDisplayStartMs;
extern bool flexQuantizeDisplayActive; extern unsigned long flexQuantizeDisplayStartMs;
extern const unsigned long OCTAVE_DISPLAY_MS;
extern const unsigned long SPREAD_DISPLAY_MS;
extern const unsigned long SCALE_DISPLAY_MS;
extern const unsigned long QUANTIZE_DISPLAY_MS;

// PC preset flash
extern bool pcPresetDisplayActive;
extern unsigned long pcPresetDisplayStartMs;
extern const unsigned long PC_PRESET_DISPLAY_MS;

// Patch save/load confirmation flash (300 ms gradient bar).
extern bool patchConfirmActive;
extern unsigned long patchConfirmStartMs;
extern const unsigned long PATCH_CONFIRM_MS;
extern int  patchConfirmKind; // 0 = save (orange->cyan), 1 = load (green->cyan)

// Battery
extern bool batteryDisplayActive;
extern unsigned long batteryDisplayStartMs;
extern const unsigned long BATTERY_DISPLAY_MS;
extern const unsigned long BATTERY_BLINK_MS;
extern int   batteryBootBars;
extern bool  batteryBootCharging;
extern float batteryBootVoltage;

// Tap tempo
extern bool tapTempoActive;extern bool tapTempoConfirming;
extern int  tapTempoTransposeMode;
// Flex-range editor (tap-tempo sub-menu)
extern bool flexRangeEditActive;
extern int  flexRangeEditTarget; // 0=none,1=min,2=max
extern bool ccSwapped;
extern int  minNote;
extern int  maxNote;
extern unsigned long led4FlashUntilMs;
extern const unsigned long TAP_BLINK_MIN_PERIOD_MS;

// Orange flash
extern bool orangeFlashActive;
extern unsigned long orangeFlashEndMs;
extern const unsigned long ORANGE_FLASH_MS;

// Flex bar
extern bool indexHeld;
extern float flexBarBend;
extern unsigned long flexPulseStartMs;
extern float flexPulseBaseW;
extern const unsigned long FLEX_PULSE_MS;

// Functions owned by main.cpp
void setTempo(int bpm);
void triggerFlexPulse();

// Battery accessor class (defined in main.cpp); we only need to call its
// methods, so a matching declaration plus an extern instance suffice.
class Xiao {
public:
  Xiao() {}
  void begin();
  float GetBatteryVoltage();
  bool IsChargingBattery();
};
extern Xiao xiao;

// Tap-tempo state owned by main.cpp.
extern int tapCount;
extern const int TAP_TEMPO_TAPS_REQUIRED;
extern unsigned long tapTimes[];
extern unsigned long lastTapMs;
extern unsigned long tapTempoConfirmEndMs;
extern const unsigned long TAP_TEMPO_TIMEOUT_MS;
extern const unsigned long TAP_TEMPO_CONFIRM_MS;
extern const unsigned long TAP_TEMPO_LED4_FLASH_MS;
extern bool externalClockActive;

// ---------------------------------------------------------------------------
// Module-owned animation state.
// ---------------------------------------------------------------------------
FingerAnim fingerAnims[4] = {
    {ANIM_IDLE, 0, 0, 0, 0, 0, false, 0, 0, 0, 0},
    {ANIM_IDLE, 0, 2, 0, 0, 0, false, 0, 0, 0, 0},
    {ANIM_IDLE, 0, 4, 0, 0, 0, false, 0, 0, 0, 0},
    {ANIM_IDLE, 0, 6, 0, 0, 0, false, 0, 0, 0, 0}
};


// ---------- Finger-driven note animation system ----------
//
// LED layout along the knuckles (0-indexed in the strip):
//   strip index : 0      1      2      3      4      5      6
//   role        : index  i/m    middle m/r    ring   r/p    pinky
//
// Source LED per finger (used as the ripple origin):
//   index  -> 0,   middle -> 2,   ring -> 4,   pinky -> 6
//
// Pitch mapping (low to high, lowest note = pinky base):
//   pinky base->tip   = Keys[0..2]   = mux 14, 13, 12
//   ring  base->tip   = Keys[3..5]   = mux 11, 10, 9
//   middle base->tip  = Keys[6..8]   = mux 8, 7, 6
//   index base->tip   = Keys[9..11]  = mux 5, 4, 3
//   (NoteOn() uses Keys[14 - channel] to translate channel -> pitch.)
//
// Per-finger color by position on the finger:
//   base   = blue   (0, 0, 255)
//   middle = cyan   (0, 255, 255)
//   tip    = purple (160, 0, 255)





// Animation timing constants
const unsigned long RIPPLE_DURATION_MS = 500;

// Ripple color fade-in: the ripple's note color fades up from 0 to full over
// RIPPLE_FADE_IN_MS at the start of the ripple, rather than appearing at full
// intensity instantly.
const unsigned long RIPPLE_FADE_IN_MS = 200;

// Off-fade (note release), two phases over OFF_FADE_DURATION_MS total:
//   0..OFF_FADE_RED_FULL_MS   : crossfade held red (R255/W50) -> ripple color
//   OFF_FADE_RED_FULL_MS..end : ripple color -> black
const unsigned long OFF_FADE_DURATION_MS = 300;
const unsigned long OFF_FADE_RED_FULL_MS = 200;  // 200 ms crossfade, then 100 ms fade-out

const unsigned long LED_FRAME_INTERVAL_MS = 16; // ~60 Hz refresh
unsigned long lastLedFrameMs = 0;

// Map a mux channel (3..14 for note contacts) to a finger and its ripple color.
// Channel-to-physical mapping (pitch order: ch14 lowest -> ch3 highest):
//   pinky: 14=base, 13=mid, 12=tip   -> lowest pitch finger
//   ring:  11=base, 10=mid,  9=tip
//   middle: 8=base,  7=mid,  6=tip
//   index:  5=base,  4=mid,  3=tip   -> highest pitch finger
//
// Ripple colors form a 12-step hue wheel from red (lowest pitch, ch14) to rose
// (highest pitch, ch3). The source knuckle LED itself is rendered pure white
// (via the RGBW W channel) on note-on; these colors are carried by the ripple.
//   ch14 red        (255,   0,   0)
//   ch13 orange     (255, 128,   0)
//   ch12 yellow     (255, 255,   0)
//   ch11 chartreuse (128, 255,   0)
//   ch10 green      (  0, 255,   0)
//   ch9  spring     (  0, 255, 128)
//   ch8  cyan       (  0, 255, 255)
//   ch7  azure      (  0, 128, 255)
//   ch6  blue       (  0,   0, 255)
//   ch5  violet     (128,   0, 255)
//   ch4  magenta    (255,   0, 255)
//   ch3  rose       (255,   0, 128)
static const uint8_t NOTE_RIPPLE_COLORS[12][3] = {
  {255,   0,   0}, // ch14 red
  {255, 128,   0}, // ch13 orange
  {255, 255,   0}, // ch12 yellow
  {128, 255,   0}, // ch11 chartreuse
  {  0, 255,   0}, // ch10 green
  {  0, 255, 128}, // ch9  spring green
  {  0, 255, 255}, // ch8  cyan
  {  0, 128, 255}, // ch7  azure
  {  0,   0, 255}, // ch6  blue
  {128,   0, 255}, // ch5  violet
  {255,   0, 255}, // ch4  magenta
  {255,   0, 128}, // ch3  rose
};

// Returns true if channel is a note channel; fills outFinger and out r/g/b
// (the ripple color). Color index = channel - 3, then reversed so ch14 -> 0.
bool channelToFingerAndColor(int channel, FingerId& outFinger, uint8_t& r, uint8_t& g, uint8_t& b)
{
  if (channel >= 3 && channel <= 5)
    outFinger = FINGER_INDEX;
  else if (channel >= 6 && channel <= 8)
    outFinger = FINGER_MIDDLE;
  else if (channel >= 9 && channel <= 11)
    outFinger = FINGER_RING;
  else if (channel >= 12 && channel <= 14)
    outFinger = FINGER_PINKY;
  else
    return false;

  // ch3 (highest) -> index 11 (rose); ch14 (lowest) -> index 0 (red).
  int idx = 14 - channel; // ch14->0 ... ch3->11
  if (idx < 0) idx = 0;
  if (idx > 11) idx = 11;
  r = NOTE_RIPPLE_COLORS[idx][0];
  g = NOTE_RIPPLE_COLORS[idx][1];
  b = NOTE_RIPPLE_COLORS[idx][2];
  return true;
}

uint8_t sourceLedForFinger(FingerId f)
{
  switch (f) {
  case FINGER_INDEX:
    return 0;
  case FINGER_MIDDLE:
    return 2;
  case FINGER_RING:
    return 4;
  case FINGER_PINKY:
    return 6;
  }
  return 0;
}

// Called from NoteOn(): (re)start the animation for the finger this channel
// belongs to. Replaces any in-flight animation on the SAME finger per spec.
// Also (re)starts the independent ripple animation so even a brief tap plays
// the full outward wave.
void TriggerFingerNoteOn(int channel)
{
  FingerId f;
  uint8_t r, g, b;
  if (!channelToFingerAndColor(channel, f, r, g, b))
    return;

  FingerAnim& a = fingerAnims[f];
  unsigned long now = millis();
  a.phase = ANIM_NOTE_ON;
  a.startMs = now;
  a.sourceLed = sourceLedForFinger(f);
  a.r = r;
  a.g = g;
  a.b = b;

  // Kick off the independent ripple
  a.rippleActive = true;
  a.rippleStartMs = now;
  a.rippleR = r;
  a.rippleG = g;
  a.rippleB = b;
}

// Called from NoteOff(): start the red-fade on the source LED for this finger.
void TriggerFingerNoteOff(int channel)
{
  FingerId f;
  uint8_t r, g, b;
  if (!channelToFingerAndColor(channel, f, r, g, b))
    return;

  FingerAnim& a = fingerAnims[f];
  if (a.phase == ANIM_NOTE_ON || a.phase == ANIM_HELD) {
    a.phase = ANIM_NOTE_OFF;
    a.startMs = millis();
    // Keep a.r/g/b as the held color so the fade-from color is correct
  }
}

// Activate the 700 ms octave indicator. While active, UpdateFingerLeds() paints
// the octave display instead of finger animations. The display is keyed off
// the current `octave` global value (set BEFORE calling this).
void TriggerOctaveDisplay()
{
  octaveDisplayStartMs = millis();
  octaveDisplayActive = true;
}

// Render the octave indicator into a flat RGB buffer (overwrites it).
// LED layout (0-indexed): index=0 ... pinky=6. Centered on LED 3.
//   LED 3 = yellow center reference (always lit while showing).
//   Down octaves grow toward the pinky (red):
//     -1 -> LED 4        -2 -> LEDs 4,5        -3 -> LEDs 4,5,6
//   Up octaves grow toward the index (green):
//     +1 -> LED 2        +2 -> LEDs 2,1        +3 -> LEDs 2,1,0
// octave index 0..6 maps to -3..+3 (octave 3 = no shift).
static void renderOctaveDisplay(uint8_t out[7][3])
{
  // Black-out everything first.
  for (int i = 0; i < 7; i++) {
    out[i][0] = 0;
    out[i][1] = 0;
    out[i][2] = 0;
  }

  // LED 3 center reference: yellow.
  out[3][0] = 255;
  out[3][1] = 255;
  out[3][2] = 0;

  int shift = octave - OCTAVE_DEFAULT; // -3..+3

  if (shift < 0) {
    // Down: light LEDs 4,5,6 (red), one per octave below center.
    int n = -shift; // 1..3
    for (int i = 0; i < n; i++) {
      int led = 4 + i; // 4, 5, 6
      if (led > 6) break;
      out[led][0] = 255; // red
      out[led][1] = 0;
      out[led][2] = 0;
    }
  } else if (shift > 0) {
    // Up: light LEDs 2,1,0 (green), one per octave above center.
    int n = shift; // 1..3
    for (int i = 0; i < n; i++) {
      int led = 2 - i; // 2, 1, 0
      if (led < 0) break;
      out[led][0] = 0;
      out[led][1] = 255; // green
      out[led][2] = 0;
    }
  }
}

// -----------------------------------------------------------------------------
// Spread display: one cyan LED per spread value (1..4), all others off.
//   spread 1 -> LED 6 cyan
//   spread 2 -> LED 4 cyan
//   spread 3 -> LED 2 cyan
//   spread 4 -> LED 0 cyan
// -----------------------------------------------------------------------------

void TriggerSpreadDisplay()
{
  spreadDisplayStartMs = millis();
  spreadDisplayActive = true;
}

static void renderSpreadDisplay(uint8_t out[7][3])
{
  for (int i = 0; i < 7; i++) {
    out[i][0] = 0;
    out[i][1] = 0;
    out[i][2] = 0;
  }

  int led = -1;
  switch (Spread) {
    case 1: led = 6; break;
    case 2: led = 4; break;
    case 3: led = 2; break;
    case 4: led = 0; break;
    default: return; // unexpected; leave strip dark
  }
  // Cyan = (0, 255, 255)
  out[led][1] = 255;
  out[led][2] = 255;
}

// -----------------------------------------------------------------------------
// Scale display: number of lit LEDs grows with scale value, color depends on
// the band the scale falls into:
//   scales 0..6  : pink   (255, 80, 180), 1..7 LEDs lit starting from LED 6
//   scales 7..13 : yellow (255, 255, 0),  1..7 LEDs lit starting from LED 6
//   scales 14..19: orange (255,  80, 0),  1..6 LEDs lit starting from LED 6
// -----------------------------------------------------------------------------

void TriggerScaleDisplay()
{
  scaleDisplayStartMs = millis();
  scaleDisplayActive = true;
}

static void renderScaleDisplay(uint8_t out[7][3])
{
  for (int i = 0; i < 7; i++) {
    out[i][0] = 0;
    out[i][1] = 0;
    out[i][2] = 0;
  }

  int count = 0;
  uint8_t r = 0, g = 0, b = 0;

  if (Scale <= 6) {
    // Red band: scale 0 = 1 LED, scale 6 = 7 LEDs (full strip).
    count = (int)Scale + 1;
    r = 255; g = 0; b = 0;
  } else if (Scale <= 13) {
    // Yellow band: scale 7 = 1 LED, scale 13 = 7 LEDs.
    count = (int)Scale - 6;
    r = 255; g = 255; b = 0;
  } else if (Scale <= 19) {
    // Blue band: scale 14 = 1 LED, scale 19 = 6 LEDs (stops at LED 1).
    count = (int)Scale - 13;
    r = 0; g = 0; b = 255;
  } else {
    return; // out of range, leave dark
  }

  if (count > 7) count = 7;

  // Light LEDs from LED 6 leftward (LED 6, then 5, then 4, ...).
  for (int i = 0; i < count; i++) {
    int led = 6 - i;
    if (led < 0) break;
    out[led][0] = r;
    out[led][1] = g;
    out[led][2] = b;
  }
}

// -----------------------------------------------------------------------------
// Combined quantize display (preset-browser sub-view, shown when ch9/ch12 are
// pressed). Note and flex quantize are shown simultaneously; LED 3 stays blank.
//
// Note quantize (purple bars from LED 6 leftward), OFF shown as an orange marker:
//   OFF  -> LED 6 orange
//   1/8  -> LED 6 purple
//   1/16 -> LEDs 6,5 purple
//   1/32 -> LEDs 6,5,4 purple
//
// Flex quantize (cyan bars from LED 2 leftward), OFF shown as a yellow marker:
//   OFF  -> LED 2 yellow
//   1/8  -> LED 2 cyan
//   1/16 -> LEDs 2,1 cyan
//   1/32 -> LEDs 2,1,0 cyan
// -----------------------------------------------------------------------------
static void renderQuantizeDisplay(uint8_t out[7][3])
{
  for (int i = 0; i < 7; i++) {
    out[i][0] = 0;
    out[i][1] = 0;
    out[i][2] = 0;
  }

  // Note: LEDs 6/5/4.
  if (noteQuantizeMode == NQ_OFF) {
    out[6][0] = 255; out[6][1] = 80; out[6][2] = 0; // orange
  } else {
    int n = (noteQuantizeMode == NQ_1_8) ? 1 : (noteQuantizeMode == NQ_1_16) ? 2 : 3;
    for (int i = 0; i < n; i++) {
      int led = 6 - i; // 6, then 5, then 4
      out[led][0] = 160; out[led][1] = 0; out[led][2] = 255; // purple
    }
  }

  // Flex: LEDs 2/1/0.
  if (flexQuantizeMode == FQ_OFF) {
    out[2][0] = 255; out[2][1] = 255; out[2][2] = 0; // yellow
  } else {
    int n = (flexQuantizeMode == FQ_1_8) ? 1 : (flexQuantizeMode == FQ_1_16) ? 2 : 3;
    for (int i = 0; i < n; i++) {
      int led = 2 - i; // 2, then 1, then 0
      out[led][0] = 0; out[led][1] = 255; out[led][2] = 255; // cyan
    }
  }
  // LED 3 intentionally left blank.
}

// -----------------------------------------------------------------------------
// Preset-browser display: the selected preset (0..62) shown as one of 63 color
// bands. The 63 = 7 LED-count steps (1..7 LEDs lit, filling from LED 6 leftward)
// x 9 colors. Color index = preset / 7, LED count = (preset % 7) + 1.
//
// Colors (index 0..8): purple, pink, blue, cyan, green, yellow, orange, red,
// white. White (index 8) is rendered via the RGBW W channel; all others use RGB.
// -----------------------------------------------------------------------------
static void renderPresetBrowserDisplay(uint8_t outRgb[7][3], uint8_t outW[7])
{
  for (int i = 0; i < 7; i++) {
    outRgb[i][0] = 0; outRgb[i][1] = 0; outRgb[i][2] = 0;
    outW[i] = 0;
  }

  // 8 RGB colors; index 8 (white) is handled via the W channel instead.
  static const uint8_t palette[8][3] = {
    {160,   0, 255}, // 0 purple
    {255,  80, 180}, // 1 pink
    {  0,   0, 255}, // 2 blue
    {  0, 255, 255}, // 3 cyan
    {  0, 255,   0}, // 4 green
    {255, 255,   0}, // 5 yellow
    {255,  80,   0}, // 6 orange
    {255,   0,   0}  // 7 red
  };

  int colorIdx = selectedPreset / 7;       // 0..8
  int count = (selectedPreset % 7) + 1;     // 1..7 LEDs
  if (colorIdx > 8) colorIdx = 8;
  if (count > 7) count = 7;

  bool useWhite = (colorIdx == 8);

  for (int i = 0; i < count; i++) {
    int led = 6 - i; // fill from LED 6 leftward
    if (led < 0) break;
    if (useWhite) {
      outW[led] = 255;
    } else {
      outRgb[led][0] = palette[colorIdx][0];
      outRgb[led][1] = palette[colorIdx][1];
      outRgb[led][2] = palette[colorIdx][2];
    }
  }
}

// -----------------------------------------------------------------------------
// Brightness preview (preset-browser sub-view, shown when ch7 cycles the level).
// A fixed rainbow is painted across LEDs 6..0. The actual brightness level is
// applied globally via strip.setBrightness(), so the whole rainbow visibly
// dims or brightens as the level is cycled (off shows no light at all).
// -----------------------------------------------------------------------------
static void renderBrightnessDisplay(uint8_t out[7][3])
{
  // 7-step rainbow, LED 6 (red) -> LED 0 (violet).
  static const uint8_t rainbow[7][3] = {
    {255,   0,   0}, // LED 6 red
    {255, 128,   0}, // LED 5 orange
    {255, 255,   0}, // LED 4 yellow
    {  0, 255,   0}, // LED 3 green
    {  0, 255, 255}, // LED 2 cyan
    {  0,   0, 255}, // LED 1 blue
    {128,   0, 255}, // LED 0 violet
  };
  for (int i = 0; i < 7; i++) {
    int led = 6 - i; // rainbow[0] -> LED 6
    out[led][0] = rainbow[i][0];
    out[led][1] = rainbow[i][1];
    out[led][2] = rainbow[i][2];
  }
}

// -----------------------------------------------------------------------------
// Tempo-bend status (preset-browser sub-view, shown when ch8 toggles it).
//   Disabled -> all 7 LEDs red.
//   Enabled  -> gradient from red (LED 6) to blue (LED 0) across the strip.
// -----------------------------------------------------------------------------
static void renderTempoBendDisplay(uint8_t out[7][3])
{
  if (!tempoBendEnabled) {
    for (int i = 0; i < 7; i++) {
      out[i][0] = 255; out[i][1] = 0; out[i][2] = 0; // all red
    }
    return;
  }
  // Enabled: linear red -> blue gradient. LED 6 = red, LED 0 = blue.
  // t = 0 at LED 6, 1 at LED 0; R fades 255->0, B fades 0->255.
  for (int led = 0; led < 7; led++) {
    float t = (float)(6 - led) / 6.0f; // led6 -> 0.0, led0 -> 1.0
    out[led][0] = (uint8_t)(255.0f * (1.0f - t)); // red
    out[led][1] = 0;
    out[led][2] = (uint8_t)(255.0f * t);          // blue
  }
}

// Colors form a gradient orange -> cyan -> red from LED 0 to LED 6:
//   LED 0 = orange (255, 80, 0)
//   LED 1 = orange/cyan interp
//   LED 2 = orange/cyan interp
//   LED 3 = cyan   (0, 255, 255)
//   LED 4 = cyan/red interp
//   LED 5 = cyan/red interp
//   LED 6 = red    (255, 0, 0)
//
// On each flex NoteOn (the moment a quantized note actually fires), the W
// channel of all currently-lit LEDs pulses to flexPulseBaseW and fades to 0
// over FLEX_PULSE_MS. Pulses stack additively in triggerFlexPulse().
// -----------------------------------------------------------------------------

static void renderFlexBarDisplay(uint8_t outRgb[7][3], uint8_t outW[7])
{
  // Gradient palette: index 0 = LED 0 (first to light), index 6 = LED 6.
  static const uint8_t palette[7][3] = {
    {255,  80,   0}, // LED 0 - orange
    {170, 138,  85}, // LED 1 - orange/cyan (1/3)
    { 85, 196, 170}, // LED 2 - orange/cyan (2/3)
    {  0, 255, 255}, // LED 3 - cyan
    { 85, 170, 170}, // LED 4 - cyan/red (1/3)
    {170,  85,  85}, // LED 5 - cyan/red (2/3)
    {255,   0,   0}  // LED 6 - red
  };

  // Total bar "budget" across the 7 LEDs.
  float budget = flexBarBend * 7.0f;
  if (budget < 0.0f) budget = 0.0f;
  if (budget > 7.0f) budget = 7.0f;

  // Compute current pulse W (decays over FLEX_PULSE_MS).
  uint8_t pulseW = 0;
  if (flexPulseBaseW > 0.0f) {
    unsigned long elapsed = millis() - flexPulseStartMs;
    if (elapsed < FLEX_PULSE_MS) {
      float k = (float)elapsed / (float)FLEX_PULSE_MS;
      float w = flexPulseBaseW * (1.0f - k);
      if (w < 0.0f) w = 0.0f;
      if (w > 255.0f) w = 255.0f;
      pulseW = (uint8_t)w;
    } else {
      flexPulseBaseW = 0.0f; // pulse complete
    }
  }

  // For each strip position (0..6), figure out its position in the bar
  // and palette color. Bar fills from LED 0 (first to light) to LED 6 (last).
  // Bar position 0 = LED 0, position 6 = LED 6 -- so they're identical here.
  for (int strip_i = 0; strip_i < 7; strip_i++) {
    int barPos = strip_i;
    int paletteIdx = strip_i;

    float fill;
    if (budget >= barPos + 1.0f) {
      fill = 1.0f;
    } else if (budget > barPos) {
      fill = budget - barPos;
    } else {
      fill = 0.0f;
    }

    outRgb[strip_i][0] = (uint8_t)(palette[paletteIdx][0] * fill);
    outRgb[strip_i][1] = (uint8_t)(palette[paletteIdx][1] * fill);
    outRgb[strip_i][2] = (uint8_t)(palette[paletteIdx][2] * fill);

    // White pulse applies to LEDs that are at least partially lit. Scale the
    // pulse by fill so partially-lit LEDs get a proportionally subdued pulse.
    if (fill > 0.0f) {
      outW[strip_i] = (uint8_t)((float)pulseW * fill);
    } else {
      outW[strip_i] = 0;
    }
  }
}

// Piecewise-linear LiPo discharge curve approximation. Input: cell voltage.
// Output: estimated capacity 0.0..1.0. The curve has the characteristic LiPo
// shape: fast drop from 4.2->4.0, long plateau around 3.7-3.9, fast drop below
// 3.5. Calibrate the breakpoints if you have a better cell model.
static float lipoVoltageToCapacity(float v)
{
  // Anchor points: (voltage, capacity)
  struct Point { float v; float c; };
  static const Point pts[] = {
    {3.30f, 0.00f},
    {3.40f, 0.03f},
    {3.50f, 0.10f},
    {3.60f, 0.25f},
    {3.70f, 0.40f},
    {3.80f, 0.55f},
    {3.90f, 0.70f},
    {4.00f, 0.85f},
    {4.10f, 0.95f},
    {4.20f, 1.00f}
  };
  const int N = sizeof(pts) / sizeof(pts[0]);

  if (v <= pts[0].v) return 0.0f;
  if (v >= pts[N - 1].v) return 1.0f;

  // Find the segment containing v and interpolate.
  for (int i = 1; i < N; i++) {
    if (v < pts[i].v) {
      float t = (v - pts[i - 1].v) / (pts[i].v - pts[i - 1].v);
      return pts[i - 1].c + t * (pts[i].c - pts[i - 1].c);
    }
  }
  return 1.0f;
}

// Low-battery threshold. Out of caution against power instability at low cell
// voltages, ANY estimated capacity below this is treated as "low" and shown as
// the flashing-red indicator (1-bar state) rather than green bars. (Raised from
// the old implicit ~1/7 = 14% bar boundary up to a 20% safety margin.)
static const float BATTERY_LOW_CAPACITY = 0.20f;

// Convert capacity 0..1 to a bar count 0..7.
//   < BATTERY_LOW_CAPACITY -> 1 bar  (the flashing-red "low battery" indicator)
//   then 2..7 green bars are spread across the remaining 20%..100% range.
static int capacityToBars(float capacity)
{
  if (capacity < BATTERY_LOW_CAPACITY) return 1; // low: flashing-red state

  // Map the healthy range [BATTERY_LOW_CAPACITY .. 1.0] across 2..7 bars so the
  // green readout still uses the full strip above the low threshold.
  float t = (capacity - BATTERY_LOW_CAPACITY) / (1.0f - BATTERY_LOW_CAPACITY);
  if (t < 0.0f) t = 0.0f;
  if (t > 1.0f) t = 1.0f;
  int bars = 2 + (int)(t * 5.0f); // 2..7
  if (bars < 2) bars = 2;
  if (bars > 7) bars = 7;
  return bars;
}

// Render the battery indicator into a flat RGB buffer.
// Bars grow from LED 6 (rightmost) leftward toward LED 0.
//   1 bar          : LED 6 blinking red (4 Hz)
//   2..6 bars      : LEDs 6, 5, ... lit solid green
//   7 bars         : All LEDs solid green
// If charging, LED 6 is overridden to solid blue (capacity bars still drawn
// on LEDs 5 and below in green).
static void renderBatteryDisplay(uint8_t out[7][3], int bars, bool charging)
{
  for (int i = 0; i < 7; i++) {
    out[i][0] = 0;
    out[i][1] = 0;
    out[i][2] = 0;
  }

  if (bars <= 1) {
    // Low: LED 6 blinks red at ~4 Hz.
    bool on = ((millis() - batteryDisplayStartMs) / BATTERY_BLINK_MS) & 1;
    if (on) {
      out[6][0] = 255;
    }
  } else {
    // Solid green bars from LED 6 leftward. bars=2 -> 6,5. bars=7 -> 6..0.
    for (int i = 0; i < bars; i++) {
      int led = 6 - i;
      if (led < 0) break;
      out[led][1] = 255;
    }
  }

  // Charging overrides LED 6 to blue regardless of capacity state.
  if (charging) {
    out[6][0] = 0;
    out[6][1] = 0;
    out[6][2] = 255;
  }
}

// Call once during setup() after the NeoPixel strip has been initialized.
// Captures battery voltage and charging state, computes the bar count, and
// arms the 2-second non-blocking indicator that UpdateFingerLeds() will paint.
void TriggerBatteryDisplay()
{
  batteryBootVoltage = xiao.GetBatteryVoltage();
  batteryBootCharging = xiao.IsChargingBattery();
  float capacity = lipoVoltageToCapacity(batteryBootVoltage);
  batteryBootBars = capacityToBars(capacity);
  batteryDisplayStartMs = millis();
  batteryDisplayActive = true;

  BOOT_PRINT("Battery: ");
  BOOT_PRINT(batteryBootVoltage, 2);
  BOOT_PRINT("V  capacity=");
  BOOT_PRINT(capacity * 100.0f, 0);
  BOOT_PRINT("%  bars=");
  BOOT_PRINT(batteryBootBars);
  BOOT_PRINT("  charging=");
  BOOT_PRINTLN(batteryBootCharging ? "yes" : "no");
}

// -----------------------------------------------------------------------------
// Tap tempo
// -----------------------------------------------------------------------------

// Enter tap-tempo capture mode. Called when octave-up has been held for >= 1 s.
// All other LED animations are suppressed; LEDs 0 and 6 blink white at the
// CURRENT tempo. The user then presses octave-up 8 times to set a new tempo.
void enterTapTempoMode()
{
  tapTempoActive = true;
  tapTempoConfirming = false;
  tapCount = 0;
  lastTapMs = millis();
  led4FlashUntilMs = 0;
  tapTempoTransposeMode = 0; // neither transpose touched yet; LED 3 defaults to white
  for (int i = 0; i < TAP_TEMPO_TAPS_REQUIRED; i++) tapTimes[i] = 0;
  DBG_PRINTLN("Tap tempo: entered. Tap octave-up 8 times.");
}

// Cancel tap-tempo capture without changing tempo.
static void exitTapTempoMode()
{
  tapTempoActive = false;
  tapTempoConfirming = false;
  tapCount = 0;
  led4FlashUntilMs = 0;
  DBG_PRINTLN("Tap tempo: exited.");
}

// Record a tap during tap-tempo mode. Called from debounceButton on the press
// edge of octave-up while tapTempoActive is true (and not yet in confirm).
// On the 8th tap, computes the BPM from the 7 inter-tap intervals and switches
// to confirmation phase (blinks at new tempo for ~1.5 s, then exits).
void recordTapTempoPress()
{
  unsigned long now = millis();
  // While locked to incoming MIDI clock, tap-based tempo capture is disabled
  // (the clock governs tempo); only the transpose menu remains usable. Still
  // flash LED 4 on each octave-up press as visual acknowledgement.
  if (externalClockActive) {
    led4FlashUntilMs = now + TAP_TEMPO_LED4_FLASH_MS;
    lastTapMs = now; // keep the inactivity timer fresh so the menu stays open
    return;
  }
  tapTimes[tapCount] = now;
  tapCount++;
  lastTapMs = now;
  led4FlashUntilMs = now + TAP_TEMPO_LED4_FLASH_MS;
  DBG_PRINT("Tap ");
  DBG_PRINT(tapCount);
  DBG_PRINT("/");
  DBG_PRINTLN(TAP_TEMPO_TAPS_REQUIRED);

  if (tapCount >= TAP_TEMPO_TAPS_REQUIRED) {
    // Compute average interval over the 7 intervals.
    unsigned long totalIntervals = tapTimes[TAP_TEMPO_TAPS_REQUIRED - 1] - tapTimes[0];
    unsigned long avgIntervalMs = totalIntervals / (TAP_TEMPO_TAPS_REQUIRED - 1);
    if (avgIntervalMs < TAP_BLINK_MIN_PERIOD_MS) {
      // Way too fast -- ignore and exit.
      DBG_PRINTLN("Tap tempo: taps too fast, aborting.");
      exitTapTempoMode();
      return;
    }
    // BPM = 60000 / avg interval (each tap is one beat).
    int newBpm = (int)(60000UL / avgIntervalMs);
    setTempo(newBpm);

    // Move into confirmation phase.
    tapTempoConfirming = true;
    tapTempoConfirmEndMs = now + TAP_TEMPO_CONFIRM_MS;
  }
}

// Driven from loop(). Handles the inactivity timeout during capture and the
// confirmation-window expiry after the 8th tap.
void updateTapTempo()
{
  if (!tapTempoActive) return;
  unsigned long now = millis();

  if (tapTempoConfirming) {
    if ((long)(now - tapTempoConfirmEndMs) >= 0) {
      exitTapTempoMode();
    }
    return;
  }

  // Capture phase: if no tap for TAP_TEMPO_TIMEOUT_MS, cancel.
  if ((now - lastTapMs) >= TAP_TEMPO_TIMEOUT_MS) {
    DBG_PRINTLN("Tap tempo: timeout, no change.");
    exitTapTempoMode();
  }
}

// Render the tap-tempo display.
// LEDs 0 and 6 blink white at the current tempo's beat (full period = 60000/BPM).
//   - On for the first half, off for the second half (50% duty).
// LED 4 lights white briefly each time a tap is captured.
// All other LEDs off (unless the transpose overlay below lights them).
// During the confirmation window after the 8th tap, the blink uses the NEW
// tempo (since setTempo() already updated it) -- this is the user-visible
// "here's what you set" feedback.
//
// Transpose overlay (rootTranspose != 0):
//   +5 LEDs 2, 1 cyan        (LED 0 blank)
//   +4 LED  2 cyan
//   +3 LEDs 2, 1, 0 purple
//   +2 LEDs 2, 1 purple
//   +1 LED  2 purple
//    0 (no extra LEDs, just tempo blink)
//   -1 LED  4 yellow
//   -2 LEDs 4, 5 yellow
//   -3 LEDs 4, 5, 6 yellow
//   -4 LED  4 orange
//   -5 LEDs 4, 5 orange      (LED 6 blank)
// -----------------------------------------------------------------------------
// Tempo nudge display (tap-tempo sub-view, shown when ch2/ch5 nudge the tempo).
//
// Encodes a tempo of 1..189 BPM into a 7-LED readout:
//   LEDs 6..2 (5 LEDs) show the "ones" digit 1..10 in unary, where each LED
//   can be off / half-bright (=1) / full-bright (=2):
//     1 -> LED6 half          6 -> LED6,5,4 full + nothing
//     2 -> LED6 full          7 -> LED6,5,4 full + LED3 half
//     3 -> LED6 full + LED5 half   ...
//     ...                     10 -> LED6,5,4,3,2 all full
//   LEDs 1 and 0 carry the decade in 20-BPM colour bands. The decade index
//   (tempo / 10) is split: even decade -> LED 1 only; odd decade -> LEDs 1 and
//   0. Colour bands (decade pair -> colour):
//     0..1  purple (  0..20)    6..7  yellow (101..120)
//     2..3  pink  ( 21..40)     8..9  orange (121..140)
//     4..5  blue  ( 41..60)    10..11 red    (141..160)
//     6..7  cyan  ( 61..80)    12..13 warm white via RGB+W (161..180)
//     8..9  green ( 81..100)
// -----------------------------------------------------------------------------
static void renderTempoNudgeDisplay(uint8_t out[7][3], uint8_t outW[7])
{
  for (int i = 0; i < 7; i++) {
    out[i][0] = 0; out[i][1] = 0; out[i][2] = 0;
    outW[i] = 0;
  }

  int bpm = tempo;
  if (bpm < 1) bpm = 1;
  if (bpm > 189) bpm = 189;

  // Ones in 1..10 (treat tempo%10 == 0 as 10 so the bar reads "full ten" at the
  // top of each decade rather than empty).
  int ones = bpm % 10;
  if (ones == 0) ones = 10;

  // Light LEDs 6 down with full+partial steps until `ones` units are placed.
  // partial = 26 (~1/10), full = 255 on the W (white) channel for a clean white
  // that scales with the global brightness.
  int remaining = ones;
  for (int step = 0; step < 5 && remaining > 0; step++) {
    int led = 6 - step; // 6, 5, 4, 3, 2
    if (remaining >= 2) {
      outW[led] = 255; // full
      remaining -= 2;
    } else {
      outW[led] = 26;  // ~1/10
      remaining -= 1;
    }
  }

  // Decade & colour. decade = tempo / 10, range 0..18 for bpm 1..189.
  int decade = bpm / 10;
  int colourIdx = decade / 2; // 0..9
  bool secondHalf = (decade & 1) != 0; // odd decade -> LEDs 1 AND 0

  static const uint8_t bands[9][3] = {
    {160,   0, 255}, // 0 purple   (  0.. 20)
    {255,  80, 180}, // 1 pink     ( 21.. 40)
    {  0,   0, 255}, // 2 blue     ( 41.. 60)
    {  0, 255, 255}, // 3 cyan     ( 61.. 80)
    {  0, 255,   0}, // 4 green    ( 81..100)
    {255, 255,   0}, // 5 yellow   (101..120)
    {255,  80,   0}, // 6 orange   (121..140)
    {255,   0,   0}, // 7 red      (141..160)
    {255, 200, 140}  // 8 warm RGB component for white band (161..180)
  };
  if (colourIdx > 8) colourIdx = 8;

  // LED 1 always carries the decade colour. For the "warm white" band we also
  // add W so the LED reads warm-white rather than pure tint.
  out[1][0] = bands[colourIdx][0];
  out[1][1] = bands[colourIdx][1];
  out[1][2] = bands[colourIdx][2];
  if (colourIdx == 8) outW[1] = 120;
  if (secondHalf) {
    out[0][0] = bands[colourIdx][0];
    out[0][1] = bands[colourIdx][1];
    out[0][2] = bands[colourIdx][2];
    if (colourIdx == 8) outW[0] = 120;
  }
}

// -----------------------------------------------------------------------------
// Flex-range editor display (tap-tempo sub-menu). Shows the MIDI note number of
// the endpoint last touched (min or max) using the same numeric encoding as the
// tempo display: ones 1..10 on LEDs 6..2 (W channel), decade colour on LED 1
// (and LED 0 for the second half of a band). The white "ones" LEDs are tinted
// dimly to indicate which endpoint is being edited:
//   editing MIN -> dim blue  (B = 50)
//   editing MAX -> dim red   (R = 50)
// -----------------------------------------------------------------------------
static void renderFlexRangeDisplay(uint8_t out[7][3], uint8_t outW[7])
{
  for (int i = 0; i < 7; i++) {
    out[i][0] = 0; out[i][1] = 0; out[i][2] = 0;
    outW[i] = 0;
  }

  int note = (flexRangeEditTarget == 1) ? minNote : maxNote; // default max
  if (note < 0) note = 0;
  if (note > 127) note = 127;

  // Tint for the white "ones" LEDs: blue for min, red for max.
  uint8_t tintR = 0, tintB = 0;
  if (flexRangeEditTarget == 1) tintB = 50;  // min -> dim blue
  else                          tintR = 50;  // max -> dim red

  int ones = note % 10;
  if (ones == 0) ones = 10;
  int remaining = ones;
  for (int step = 0; step < 5 && remaining > 0; step++) {
    int led = 6 - step; // 6, 5, 4, 3, 2
    if (remaining >= 2) {
      outW[led] = 255; remaining -= 2;       // full
    } else {
      outW[led] = 26;  remaining -= 1;       // ~1/10
    }
    out[led][0] = tintR; // dim colour tint alongside the white
    out[led][2] = tintB;
  }

  int decade = note / 10;          // 0..12 for notes 0..127
  int colourIdx = decade / 2;      // 0..6
  bool secondHalf = (decade & 1) != 0;
  static const uint8_t bands[9][3] = {
    {160,   0, 255}, {255,  80, 180}, {  0,   0, 255}, {  0, 255, 255},
    {  0, 255,   0}, {255, 255,   0}, {255,  80,   0}, {255,   0,   0},
    {255, 200, 140}
  };
  if (colourIdx > 8) colourIdx = 8;
  out[1][0] = bands[colourIdx][0];
  out[1][1] = bands[colourIdx][1];
  out[1][2] = bands[colourIdx][2];
  if (colourIdx == 8) outW[1] = 120;
  if (secondHalf) {
    out[0][0] = bands[colourIdx][0];
    out[0][1] = bands[colourIdx][1];
    out[0][2] = bands[colourIdx][2];
    if (colourIdx == 8) outW[0] = 120;
  }
}

// -----------------------------------------------------------------------------
// CC-swap display (tap-tempo sub-view, shown when ch4 toggles the swap).
//   Default (not swapped) -> all 7 LEDs turquoise.
//   Swapped               -> turquoise -> purple gradient across LEDs 6..0.
// -----------------------------------------------------------------------------
static void renderCcSwapDisplay(uint8_t out[7][3])
{
  // Turquoise (0,200,180) and purple (160,0,255).
  if (!ccSwapped) {
    for (int i = 0; i < 7; i++) {
      out[i][0] = 0; out[i][1] = 200; out[i][2] = 180; // turquoise
    }
    return;
  }
  // Gradient turquoise (LED 6) -> purple (LED 0).
  for (int led = 0; led < 7; led++) {
    float t = (float)(6 - led) / 6.0f; // led6 -> 0.0 (turquoise), led0 -> 1.0 (purple)
    out[led][0] = (uint8_t)(0   * (1.0f - t) + 160 * t);
    out[led][1] = (uint8_t)(200 * (1.0f - t) +   0 * t);
    out[led][2] = (uint8_t)(180 * (1.0f - t) + 255 * t);
  }
}

static void renderTapTempoDisplay(uint8_t out[7][3])
{  for (int i = 0; i < 7; i++) {
    out[i][0] = 0;    out[i][1] = 0;
    out[i][2] = 0;
  }

  // Blink period for the current tempo, in ms per beat.
  unsigned long beatMs = 60000UL / (unsigned long)tempo;
  if (beatMs < TAP_BLINK_MIN_PERIOD_MS) beatMs = TAP_BLINK_MIN_PERIOD_MS;

  // Tempo blink on LED 3 (middle LED).
  // White = keyTranspose mode (channels 3/6) or no transpose touched yet.
  // Blue  = rootTranspose mode (channels 9/12).
  unsigned long now = millis();
  unsigned long phase = now % beatMs;
  bool on = phase < (beatMs / 2);

  // Beat colour (shared by the LED-3 blink and the outward ripple).
  uint8_t beatR, beatG, beatB;
  if (tapTempoTransposeMode == 2) {       // rootTranspose -> blue
    beatR = 0; beatG = 0; beatB = 255;
  } else {                                 // keyTranspose / default -> white
    beatR = 255; beatG = 255; beatB = 255;
  }

  // The beat blink + outward ripple only run in the DEFAULT tap-tempo state
  // (no sub-mode engaged). Once a transpose sub-mode is in use, the animation
  // is suppressed but LED 3 stays SOLID in the mode colour (white = key, blue =
  // root) so it still reads as the active-mode indicator.
  if (tapTempoTransposeMode == 0) {
    if (on) {
      out[3][0] = beatR; out[3][1] = beatG; out[3][2] = beatB;
    }

    // Outward ripple: on every beat onset, a wavefront radiates from LED 3 to
    // the edges (radius 1 -> 2 -> 3). `phase` doubles as ripple progress. Each
    // radius lights as the wavefront reaches it, then fades. Additive over the
    // blink so the centre flash and wave read as one.
    const unsigned long RIPPLE_STEP_MS = 70;  // wavefront travel time per LED
    const unsigned long RIPPLE_TAIL_MS = 140; // fade time after the front passes
    for (int radius = 1; radius <= 3; radius++) {
      unsigned long arrival = (unsigned long)radius * RIPPLE_STEP_MS;
      if (phase < arrival) continue;            // wavefront hasn't reached yet
      unsigned long since = phase - arrival;
      if (since >= RIPPLE_TAIL_MS) continue;     // already faded
      float k = (1.0f - (float)since / (float)RIPPLE_TAIL_MS) * 0.1f; // 1/10 peak
      uint8_t rr = (uint8_t)(beatR * k);
      uint8_t rg = (uint8_t)(beatG * k);
      uint8_t rb = (uint8_t)(beatB * k);
      int loLed = 3 - radius;
      int hiLed = 3 + radius;
      if (loLed >= 0) {
        if (rr > out[loLed][0]) out[loLed][0] = rr;
        if (rg > out[loLed][1]) out[loLed][1] = rg;
        if (rb > out[loLed][2]) out[loLed][2] = rb;
      }
      if (hiLed <= 6) {
        if (rr > out[hiLed][0]) out[hiLed][0] = rr;
        if (rg > out[hiLed][1]) out[hiLed][1] = rg;
        if (rb > out[hiLed][2]) out[hiLed][2] = rb;
      }
    }
  } else {
    // Sub-mode engaged: LED 3 solid in the mode colour, no blink, no ripple.
    out[3][0] = beatR; out[3][1] = beatG; out[3][2] = beatB;
  }

  // LED 3 also flashes on each octave-up tap, overriding the off-phase.
  // Color matches the current mode: white for keyTranspose, blue for rootTranspose.
  if ((long)(now - led4FlashUntilMs) < 0) {
    out[3][0] = beatR; out[3][1] = beatG; out[3][2] = beatB;
  }

  // Transpose overlay on LEDs 0/1/2 (positive) or 4/5/6 (negative).
  // Shows the ACTIVE transpose type (most recently changed).
  // Colors: purple = (160, 0, 255), cyan = (0, 255, 255),
  //         yellow = (255, 255, 0), orange = (255, 80, 0).
  int activeTranspose = (tapTempoTransposeMode == 2) ? rootTranspose : keyTranspose;
  uint8_t r = 0, g = 0, b = 0;
  int leds[3] = {-1, -1, -1}; // up to 3 LEDs to light
  switch (activeTranspose) {
    case  5: leds[0] = 2; leds[1] = 1;               r = 0;   g = 255; b = 255; break; // cyan
    case  4: leds[0] = 2;                             r = 0;   g = 255; b = 255; break; // cyan
    case  3: leds[0] = 2; leds[1] = 1; leds[2] = 0;  r = 160; g = 0;   b = 255; break; // purple
    case  2: leds[0] = 2; leds[1] = 1;               r = 160; g = 0;   b = 255; break; // purple
    case  1: leds[0] = 2;                             r = 160; g = 0;   b = 255; break; // purple
    case  0: break;
    case -1: leds[0] = 4;                             r = 255; g = 255; b = 0;   break; // yellow
    case -2: leds[0] = 4; leds[1] = 5;               r = 255; g = 255; b = 0;   break; // yellow
    case -3: leds[0] = 4; leds[1] = 5; leds[2] = 6;  r = 255; g = 255; b = 0;   break; // yellow
    case -4: leds[0] = 4;                             r = 255; g = 80;  b = 0;   break; // orange
    case -5: leds[0] = 4; leds[1] = 5;               r = 255; g = 80;  b = 0;   break; // orange
    default: break;
  }
  for (int i = 0; i < 3; i++) {
    int led = leds[i];
    if (led < 0 || led >= 7) continue;
    out[led][0] = r;
    out[led][1] = g;
    out[led][2] = b;
  }
}

// -----------------------------------------------------------------------------
// Orange flash (tap-tempo-unavailable feedback)
// -----------------------------------------------------------------------------

// Arm the 500 ms full-strip orange flash. Suppresses all other animations
// while active.
void triggerOrangeFlash()
{
  orangeFlashEndMs = millis() + ORANGE_FLASH_MS;
  orangeFlashActive = true;
  DBG_PRINTLN("Orange flash: tap tempo disabled by incoming MIDI clock");
}

// Solid orange (255, 80, 0) across all 7 LEDs.
static void renderOrangeFlash(uint8_t out[7][3])
{
  for (int i = 0; i < 7; i++) {
    out[i][0] = 255;
    out[i][1] = 80;
    out[i][2] = 0;
  }
}

// Patch save/load confirmation: a static gradient bar across the 7 LEDs.
//   kind 0 (save) : LED 0 orange -> LED 6 cyan
//   kind 1 (load) : LED 0 green  -> LED 6 cyan
// Held for PATCH_CONFIRM_MS by the dispatch logic, then released.
static void renderPatchConfirm(uint8_t out[7][3], int kind)
{
  // Start color depends on the action; both ramp to cyan at the far end.
  uint8_t sr, sg, sb;
  if (kind == 1) { sr = 0;   sg = 255; sb = 0;   } // load: green
  else           { sr = 255; sg = 110; sb = 0;   } // save: orange
  const uint8_t er = 0, eg = 200, eb = 255;        // cyan

  for (int i = 0; i < 7; i++) {
    float t = (float)i / 6.0f; // 0 at LED0, 1 at LED6
    out[i][0] = (uint8_t)(sr + t * ((int)er - (int)sr));
    out[i][1] = (uint8_t)(sg + t * ((int)eg - (int)sg));
    out[i][2] = (uint8_t)(sb + t * ((int)eb - (int)sb));
  }
}

// Gaussian-like profile for the moving ripple peak.
// Returns 0..1 brightness for an LED that is `delta` LEDs away from the peak.
// Sigma = 0.9 gives:  delta=0 -> 1.0,  delta=1 -> 0.54,  delta=2 -> 0.085
static inline float ripplePeakProfile(float delta)
{
  const float sigma2x2 = 1.62f; // 2 * 0.9^2
  return expf(-(delta * delta) / sigma2x2);
}

// Render one finger's contribution into the per-LED accumulators. RGB receives
// the outward ripple (the note's spectrum color); the W buffer receives the
// source knuckle LED, which is rendered pure WHITE on note-on (and fades out
// via W on release).
void renderFingerInto(const FingerAnim& a, float buf[7][3], float bufW[7])
{
  // Nothing to draw if there's no phase activity AND no ripple in flight.
  if (a.phase == ANIM_IDLE && !a.rippleActive)
    return;

  unsigned long now = millis();
  unsigned long elapsed = now - a.startMs;

  // Source LED while the note is alive (NOTE_ON or HELD): red with a touch of
  // white (R255 G0 B0 W50) for the entire held duration. The note's spectrum
  // color is carried only by the outward ripple, not the source knuckle LED.
  if (a.phase == ANIM_NOTE_ON || a.phase == ANIM_HELD) {
    buf[a.sourceLed][0] += 255.0f; // red
    bufW[a.sourceLed]   += 50.0f;  // slight white
  }

  // Independent ripple animation: runs for RIPPLE_DURATION_MS from its own
  // start time, regardless of phase. Even if the note was released before the
  // ripple completed, the outgoing wave still plays out fully.
  if (a.rippleActive) {
    unsigned long rippleElapsed = now - a.rippleStartMs;
    float progress = (float)rippleElapsed / (float)RIPPLE_DURATION_MS; // 0..1
    if (progress > 1.0f)
      progress = 1.0f;
    float peakDistance = progress * 6.0f;

    // Envelope so the ripple fades as it moves outward. Peak intensity scaled
    // down so it reads as a glow rather than competing with the source LED's
    // full color (prevents bright pileups on inner LEDs when multiple fingers
    // ripple simultaneously).
    // Peak intensity scaled down so the moving wave reads as a glow rather
    // than competing with the source LED's full color. Lower values make the
    // active-note source LED stand out more relative to its ripple trail.
    const float RIPPLE_PEAK_GAIN = 0.3f;
    float envelope = (1.0f - progress) * RIPPLE_PEAK_GAIN;

    // Fade the ripple color in over RIPPLE_FADE_IN_MS so it doesn't pop in at
    // full intensity at the moment the note is triggered.
    if (rippleElapsed < RIPPLE_FADE_IN_MS) {
      envelope *= (float)rippleElapsed / (float)RIPPLE_FADE_IN_MS;
    }

    for (int led = 0; led < 7; led++) {
      if (led == a.sourceLed)
        continue;
      float distFromSource = fabsf((float)led - (float)a.sourceLed);
      float delta = distFromSource - peakDistance;
      float intensity = ripplePeakProfile(delta) * envelope;
      if (intensity <= 0.0f)
        continue;
      buf[led][0] += a.rippleR * intensity;
      buf[led][1] += a.rippleG * intensity;
      buf[led][2] += a.rippleB * intensity;
    }
  }

  if (a.phase == ANIM_NOTE_OFF) {
    // Source LED only. Held color was R255 G0 B0 W50. On release it crossfades
    // to the note's ripple color, then fades that color to black.
    //   0..OFF_FADE_RED_FULL_MS   : crossfade held red (R255/W50) -> ripple color
    //   OFF_FADE_RED_FULL_MS..end : ripple color -> black
    unsigned long e = elapsed;
    if (e < OFF_FADE_RED_FULL_MS) {
      float k = (float)e / (float)OFF_FADE_RED_FULL_MS; // 0..1
      // k=0 -> held red (R255,0,0,W50); k=1 -> ripple color (rippleR,G,B, W0).
      buf[a.sourceLed][0] += 255.0f * (1.0f - k) + (float)a.rippleR * k;
      buf[a.sourceLed][1] +=   0.0f * (1.0f - k) + (float)a.rippleG * k;
      buf[a.sourceLed][2] +=   0.0f * (1.0f - k) + (float)a.rippleB * k;
      bufW[a.sourceLed]   +=  50.0f * (1.0f - k);
    } else {
      float k = (float)(e - OFF_FADE_RED_FULL_MS) /
                (float)(OFF_FADE_DURATION_MS - OFF_FADE_RED_FULL_MS); // 0..1
      if (k > 1.0f)
        k = 1.0f;
      // Ripple color fading to black.
      buf[a.sourceLed][0] += (float)a.rippleR * (1.0f - k);
      buf[a.sourceLed][1] += (float)a.rippleG * (1.0f - k);
      buf[a.sourceLed][2] += (float)a.rippleB * (1.0f - k);
    }
  }
}

// Advance phases that have completed. Called once per frame.
// - NOTE_ON -> HELD when source LED has been at full color for RIPPLE_DURATION_MS
//   (kept for backward compatibility with the old phase machinery, though now
//   the ripple itself runs on its own timer regardless of phase)
// - NOTE_OFF -> IDLE after OFF_FADE_DURATION_MS
// - rippleActive cleared after RIPPLE_DURATION_MS from its own start
void advanceFingerPhases()
{
  unsigned long now = millis();
  for (int i = 0; i < 4; i++) {
    FingerAnim& a = fingerAnims[i];
    if (a.phase == ANIM_NOTE_ON && (now - a.startMs) >= RIPPLE_DURATION_MS) {
      a.phase = ANIM_HELD;
    } else if (a.phase == ANIM_NOTE_OFF && (now - a.startMs) >= OFF_FADE_DURATION_MS) {
      a.phase = ANIM_IDLE;
    }
    if (a.rippleActive && (now - a.rippleStartMs) >= RIPPLE_DURATION_MS) {
      a.rippleActive = false;
    }
  }
}

// Per-finger routine wrappers (four separate routines as requested in the spec).
void renderIndexInto(float buf[7][3], float bufW[7])
{
  renderFingerInto(fingerAnims[FINGER_INDEX], buf, bufW);
}
void renderMiddleInto(float buf[7][3], float bufW[7])
{
  renderFingerInto(fingerAnims[FINGER_MIDDLE], buf, bufW);
}
void renderRingInto(float buf[7][3], float bufW[7])
{
  renderFingerInto(fingerAnims[FINGER_RING], buf, bufW);
}
void renderPinkyInto(float buf[7][3], float bufW[7])
{
  renderFingerInto(fingerAnims[FINGER_PINKY], buf, bufW);
}

// Screen blend two 0..255 channel values: result = a + b - a*b/255.
// Bounded at 255 by construction. Two bright sources combine to be brighter
// than either, but they don't saturate as aggressively as additive sum.
static inline uint8_t screenBlend8(int a, int b)
{
  if (a < 0) a = 0;
  if (a > 255) a = 255;
  if (b < 0) b = 0;
  if (b > 255) b = 255;
  int r = a + b - ((a * b) / 255);
  if (r > 255) r = 255;
  if (r < 0) r = 0;
  return (uint8_t)r;
}

// Top-level frame update. Throttled to ~60 Hz.
//
// Each finger renders into its own temp buffer (additively within the finger
// so source + ripple combine naturally), then the four per-finger buffers are
// combined into the master strip values via SCREEN BLEND. Screen blend is
// bounded at 255 per channel and looks much more natural than raw additive
// sum when multiple light sources overlap.
//
// We also skip the DMA push entirely if no pixel value changed since the last
// frame, and skip it if a previous DMA send is still in progress.
void UpdateFingerLeds()
{
  unsigned long now = millis();
  if ((now - lastLedFrameMs) < LED_FRAME_INTERVAL_MS)
    return;
  lastLedFrameMs = now;

  advanceFingerPhases();

  // Expire the battery boot indicator after its 2 s window.
  if (batteryDisplayActive && (now - batteryDisplayStartMs) >= BATTERY_DISPLAY_MS) {
    batteryDisplayActive = false;
  }

  // Expire the octave indicator if its 700 ms window has elapsed.
  if (octaveDisplayActive && (now - octaveDisplayStartMs) >= OCTAVE_DISPLAY_MS) {
    octaveDisplayActive = false;
  }

  // Expire the spread indicator.
  if (spreadDisplayActive && (now - spreadDisplayStartMs) >= SPREAD_DISPLAY_MS) {
    spreadDisplayActive = false;
  }

  // Expire the scale indicator -- but keep it lit for the whole time scale-edit
  // mode is active (it only fades on its 700 ms timer when NOT editing, e.g.
  // when shown briefly on entering/leaving the latch).
  if (scaleDisplayActive && !scaleEditActive &&
      (now - scaleDisplayStartMs) >= SCALE_DISPLAY_MS) {
    scaleDisplayActive = false;
  }

  // Expire the quantize-mode indicator.
  if (quantizeDisplayActive && (now - quantizeDisplayStartMs) >= QUANTIZE_DISPLAY_MS) {
    quantizeDisplayActive = false;
  }

  // Expire the flex-quantize-mode indicator.
  if (flexQuantizeDisplayActive && (now - flexQuantizeDisplayStartMs) >= QUANTIZE_DISPLAY_MS) {
    flexQuantizeDisplayActive = false;
  }

  // Expire the orange "tap-tempo unavailable" flash.
  if (orangeFlashActive && (long)(now - orangeFlashEndMs) >= 0) {
    orangeFlashActive = false;
  }

  // Expire the PC-driven preset-bar override.
  if (pcPresetDisplayActive && (now - pcPresetDisplayStartMs) >= PC_PRESET_DISPLAY_MS) {
    pcPresetDisplayActive = false;
  }

  // Expire the patch save/load confirmation flash.
  if (patchConfirmActive && (now - patchConfirmStartMs) >= PATCH_CONFIRM_MS) {
    patchConfirmActive = false;
  }

  static bool lastFrameWasBlack = false;
  static uint8_t prevPix[7][3] = {{0}};
  static uint8_t curPix[7][3];
  // W-channel companion buffers. Used only by the flex bar's note pulse
  // animation; all other branches leave these at zero.
  static uint8_t prevPixW[7] = {0};
  static uint8_t curPixW[7] = {0};
  for (int i = 0; i < 7; i++) curPixW[i] = 0;
  bool changed = false;
  bool anyNonZero = false;

  if (patchConfirmActive) {
    // Top priority: brief 300 ms gradient bar confirming a patch save or load.
    // Save = orange->cyan, Load = green->cyan.
    renderPatchConfirm(curPix, patchConfirmKind);
    for (int i = 0; i < 7; i++) {
      if (curPix[i][0] != prevPix[i][0] || curPix[i][1] != prevPix[i][1] ||
          curPix[i][2] != prevPix[i][2] || curPixW[i] != prevPixW[i]) {
        changed = true;
      }
      if (curPix[i][0] || curPix[i][1] || curPix[i][2] || curPixW[i]) anyNonZero = true;
    }
  } else if (pcPresetDisplayActive) {
    // Highest priority: brief preset-bar shown when a MIDI Program Change loads
    // a preset. Overrides all other LED states for PC_PRESET_DISPLAY_MS. Uses
    // the same 63-color-band rendering as the browser (reads selectedPreset,
    // which was set to the loaded slot).
    renderPresetBrowserDisplay(curPix, curPixW);
    for (int i = 0; i < 7; i++) {
      if (curPix[i][0] != prevPix[i][0] || curPix[i][1] != prevPix[i][1] ||
          curPix[i][2] != prevPix[i][2] || curPixW[i] != prevPixW[i]) {
        changed = true;
      }
      if (curPix[i][0] || curPix[i][1] || curPix[i][2] || curPixW[i]) anyNonZero = true;
    }
  } else if (orangeFlashActive) {
    // Highest priority: full-strip orange flash signaling tap-tempo is
    // disabled because external MIDI clock is locked.
    renderOrangeFlash(curPix);
    for (int i = 0; i < 7; i++) {
      if (curPix[i][0] != prevPix[i][0] || curPix[i][1] != prevPix[i][1] ||
          curPix[i][2] != prevPix[i][2]) {
        changed = true;
      }
      if (curPix[i][0] || curPix[i][1] || curPix[i][2]) anyNonZero = true;
    }
  } else if (tapTempoActive) {
    // Tap-tempo display takes ABSOLUTE top priority over everything else.
    // The flex-range editor (sub-menu) overrides the rest; otherwise the tempo
    // nudge readout shows when that was the most recent input, else the
    // transpose/blink display.
    if (flexRangeEditActive) {
      renderFlexRangeDisplay(curPix, curPixW);
    } else if (tapTempoTransposeMode == 4) {
      renderCcSwapDisplay(curPix);
    } else if (tapTempoTransposeMode == 3) {
      renderTempoNudgeDisplay(curPix, curPixW);
    } else {
      renderTapTempoDisplay(curPix);
    }
    for (int i = 0; i < 7; i++) {
      if (curPix[i][0] != prevPix[i][0] || curPix[i][1] != prevPix[i][1] ||
          curPix[i][2] != prevPix[i][2] || curPixW[i] != prevPixW[i]) {
        changed = true;
      }
      if (curPix[i][0] || curPix[i][1] || curPix[i][2] || curPixW[i]) anyNonZero = true;
    }
  } else if (batteryDisplayActive) {
    // Boot battery indicator takes top priority. Captured voltage/charging
    // state is held for the whole 2 s window; only the blink phase animates.
    renderBatteryDisplay(curPix, batteryBootBars, batteryBootCharging);
    for (int i = 0; i < 7; i++) {
      if (curPix[i][0] != prevPix[i][0] || curPix[i][1] != prevPix[i][1] ||
          curPix[i][2] != prevPix[i][2]) {
        changed = true;
      }
      if (curPix[i][0] || curPix[i][1] || curPix[i][2]) anyNonZero = true;
    }
  } else if (presetBrowserActive) {
    // Preset-browser display: persistent while browsing. Sub-views:
    //   presetSubView 0 -> preset color band (selected slot), set on entry and
    //                      whenever ch3/ch6 scroll the selection.
    //   presetSubView 1 -> combined flex+note quantize display, set whenever
    //                      ch9/ch12 change a quantize mode.
    //   presetSubView 2 -> brightness rainbow preview, set whenever ch7 cycles
    //                      the brightness level.
    //   presetSubView 3 -> tempo-bend status (all red = off, red->blue = on),
    //                      set whenever ch8 toggles it.
    if (presetSubView == 1) {
      renderQuantizeDisplay(curPix);
    } else if (presetSubView == 2) {
      renderBrightnessDisplay(curPix);
    } else if (presetSubView == 3) {
      renderTempoBendDisplay(curPix);
    } else {
      renderPresetBrowserDisplay(curPix, curPixW);
    }
    for (int i = 0; i < 7; i++) {
      if (curPix[i][0] != prevPix[i][0] || curPix[i][1] != prevPix[i][1] ||
          curPix[i][2] != prevPix[i][2] || curPixW[i] != prevPixW[i]) {
        changed = true;
      }
      if (curPix[i][0] || curPix[i][1] || curPix[i][2] || curPixW[i]) anyNonZero = true;
    }
  } else if (scaleEditActive || octaveDisplayActive || spreadDisplayActive ||
             scaleDisplayActive || quantizeDisplayActive) {
    // Modal indicators (octave / spread / scale / quantize): most-recent-wins.
    // Whichever was triggered most recently takes the strip until its 700 ms
    // window ends; the others, if also active, are hidden underneath.
    // While scale-edit mode is active, the scale indicator is forced on and
    // held for the whole session (note animations stay suppressed underneath).
    unsigned long pickStart = 0;
    int pickKind = 0; // 1=octave, 2=spread, 3=scale, 4=quantize
    if (octaveDisplayActive && octaveDisplayStartMs >= pickStart) {
      pickStart = octaveDisplayStartMs;
      pickKind = 1;
    }
    if (spreadDisplayActive && spreadDisplayStartMs >= pickStart) {
      pickStart = spreadDisplayStartMs;
      pickKind = 2;
    }
    if (scaleDisplayActive && scaleDisplayStartMs >= pickStart) {
      pickStart = scaleDisplayStartMs;
      pickKind = 3;
    }
    if (quantizeDisplayActive && quantizeDisplayStartMs >= pickStart) {
      pickStart = quantizeDisplayStartMs;
      pickKind = 4;
    }
    if (scaleEditActive) pickKind = 3; // scale-edit always shows the scale bar
    if (pickKind == 1) renderOctaveDisplay(curPix);
    else if (pickKind == 2) renderSpreadDisplay(curPix);
    else if (pickKind == 4) renderQuantizeDisplay(curPix);
    else renderScaleDisplay(curPix); // default incl. pickKind==3 / scale-edit

    for (int i = 0; i < 7; i++) {
      if (curPix[i][0] != prevPix[i][0] || curPix[i][1] != prevPix[i][1] ||
          curPix[i][2] != prevPix[i][2]) {
        changed = true;
      }
      if (curPix[i][0] || curPix[i][1] || curPix[i][2]) anyNonZero = true;
    }
  } else if (indexHeld) {
    // Flex bar display: progressive bar from LED 6 (least bent) toward LED 0
    // (fully bent), gradient blue -> cyan -> purple. The bar suppresses the
    // per-finger note animations while the index modal contact is held.
    // White-channel pulses fire at flex NoteOn events (see triggerFlexPulse).
    renderFlexBarDisplay(curPix, curPixW);
    for (int i = 0; i < 7; i++) {
      if (curPix[i][0] != prevPix[i][0] || curPix[i][1] != prevPix[i][1] ||
          curPix[i][2] != prevPix[i][2] || curPixW[i] != prevPixW[i]) {
        changed = true;
      }
      if (curPix[i][0] || curPix[i][1] || curPix[i][2] || curPixW[i]) anyNonZero = true;
    }
  } else {
    // Fast path: if all four fingers are idle (no phase activity AND no ripple
    // in flight), only push a black frame once.
    bool allIdle = true;
    for (int i = 0; i < 4; i++) {
      if (fingerAnims[i].phase != ANIM_IDLE || fingerAnims[i].rippleActive) {
        allIdle = false;
        break;
      }
    }
    if (allIdle && lastFrameWasBlack) {
      return;
    }

    // One buffer per finger. Kept in static storage (not on the stack) to keep
    // the main loop task's stack footprint small -- a deep call chain with
    // BLE/I2C activity plus a 336-byte local float buffer is asking for trouble.
    static float fbuf[4][7][3];
    static float fbufW[4][7];
    for (int f = 0; f < 4; f++) {
      for (int i = 0; i < 7; i++) {
        fbuf[f][i][0] = 0.0f;
        fbuf[f][i][1] = 0.0f;
        fbuf[f][i][2] = 0.0f;
        fbufW[f][i] = 0.0f;
      }
    }
    renderFingerInto(fingerAnims[FINGER_INDEX],  fbuf[0], fbufW[0]);
    renderFingerInto(fingerAnims[FINGER_MIDDLE], fbuf[1], fbufW[1]);
    renderFingerInto(fingerAnims[FINGER_RING],   fbuf[2], fbufW[2]);
    renderFingerInto(fingerAnims[FINGER_PINKY],  fbuf[3], fbufW[3]);

    // Combine across fingers using screen blend so cross-finger overlap can't
    // overshoot 255. Track whether the result differs from the previous frame;
    // if not, skip the DMA push. The W channel (source-LED white) is combined
    // the same way.
    for (int i = 0; i < 7; i++) {
      uint8_t r = 0, g = 0, b = 0, w = 0;
      for (int f = 0; f < 4; f++) {
        r = screenBlend8(r, (int)fbuf[f][i][0]);
        g = screenBlend8(g, (int)fbuf[f][i][1]);
        b = screenBlend8(b, (int)fbuf[f][i][2]);
        w = screenBlend8(w, (int)fbufW[f][i]);
      }
      curPix[i][0] = r;
      curPix[i][1] = g;
      curPix[i][2] = b;
      curPixW[i] = w;
      if (r != prevPix[i][0] || g != prevPix[i][1] || b != prevPix[i][2] ||
          w != prevPixW[i]) {
        changed = true;
      }
      if (r != 0 || g != 0 || b != 0 || w != 0) {
        anyNonZero = true;
      }
    }
  }

  if (ledForceRepush) {
    changed = true;        // brightness changed; force a re-push this frame
    ledForceRepush = false;
  }

  if (!changed) {
    // Nothing to push -- pixels unchanged since last frame.
    return;
  }

  // Push pixels. Library-level brightness scaling (set via strip.setBrightness)
  // is applied internally by Adafruit_NeoPixel. The W channel is normally 0;
  // the flex bar branch uses it for the note-trigger pulse.
  for (int i = 0; i < 7; i++) {
    strip.setPixelColor(i, strip.Color(curPix[i][0], curPix[i][1], curPix[i][2], curPixW[i]));
    prevPix[i][0] = curPix[i][0];
    prevPix[i][1] = curPix[i][1];
    prevPix[i][2] = curPix[i][2];
    prevPixW[i] = curPixW[i];
  }
  strip.show();
  // Drive the data pin LOW after show() finishes. Adafruit_NeoPixel on nRF52840
  // disconnects the PWM peripheral from the pin after DMA completes, which
  // leaves the pin floating. A floating data line between frames can pick up
  // noise (BLE radio, capacitive coupling) that the SK6812 may interpret as
  // spurious data bits, latching wrong colors into individual LEDs. Holding
  // the line LOW between refreshes prevents that.
  clampDataPinLow();
  lastFrameWasBlack = !anyNonZero;
}

// ---------- end finger animation system ----------

// Helper to drive the LED data pin LOW after show() to prevent floating-pin
// noise from being interpreted as spurious data bits between frames.
static inline void clampDataPinLow()
{
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
}


