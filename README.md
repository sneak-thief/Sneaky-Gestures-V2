# Sneaky Gestures V2: Glove MIDI Controller

A wearable BLE-MIDI gestural instrument. The thumb acts as a common electrode
that touches conductive pads on the other fingers to play notes; a flex sensor
in the index finger bends pitch, a thumb FSR sends aftertouch, and the
accelerometer streams two continuous MIDI CCs. A 7-LED RGBW strip on the back
of the hand displays the current mode at a glance.

Built on a **Seeed XIAO nRF52840 Sense**. Transmits as a standard BLE-MIDI
device — pair it with any DAW, hardware synth, or mobile MIDI host.

---

## Features

- **12 finger pads** (3 per finger × 4 fingers) playing scale-quantized MIDI
- **20 scales** including major, minor, modes, pentatonic, blues, exotic
  modes — selectable live
- **Flex-bend** continuous pitch sweep mapped across the configured note range,
  scale-quantized
- **Two-axis accelerometer CCs** — CC74 / CC1 by default, swappable
- **Channel aftertouch** from the thumb FSR
- **Tap tempo** with sub-menus for key/root transpose, BPM nudge, CC swap, and
  the flex-range editor — works alongside incoming MIDI clock
- **Tempo-tracking pitch bend** — automatically retunes to the host's clock
  so samples stay in pitch when the project tempo changes
- **63 presets** persisted to onboard flash; MIDI Program Change recalls them
- **Per-channel and global stuck-note prevention** for reliable BLE-MIDI
- **Quantization grids** for both finger notes and flex notes (off / 1/8 /
  1/16 / 1/32), locked to internal tempo or external MIDI clock

## Hardware

| Component       | Where                            | Connection                        |
|-----------------|----------------------------------|-----------------------------------|
| MCU             | Wrist                            | Seeed XIAO nRF52840 Sense         |
| LED strip       | Back of hand                     | 7 × SK681 RGBW on pin 5           |
| Multiplexer     | Inline                           | CD74HC4067, S0–S3 on pins 9/8/7/6 |
| Touch pads      | 12 on the fingers + thumb common | Mux inputs                        |
| Flex sensor     | Index finger back                | Analog A2                         |
| FSR             | Thumb pad                        | Analog A1                         |
| IMU             | Onboard                          | LSM6DS3 (Sense board), I²C 0x6A   |
| Battery         | LiPo, JST connector              | `PIN_VBAT` monitor                |
| Boost Converter | Underneath the SK6812            | 5V regulator for the SK6812       |

See `glove_layout.html` for a labelled diagram of pad and channel positions.

## LED display

The strip is the only on-device feedback. Each mode repaints it with a distinct
meaning — notes ripple outward from the played knuckle, the flex bar fills as
the finger bends, octave/spread/scale flash brief indicators, and the tap-tempo
and preset-browser modes each host several sub-views with their own LED layouts.

The full visual reference is in [LED_Mode_Reference.html](https://htmlpreview.github.io/?https://github.com/sneak-thief/Sneaky-Gestures-V2/blob/main/documentation/LED_Mode_Reference.html)

## Build

This is a [PlatformIO](https://platformio.org/) project targeting the Adafruit
nRF52 BSP.

```sh
# Build
pio run -e seeed_xiao_nrf52840_sense

# Upload (put the XIAO in bootloader mode — double-tap reset)
pio run -e seeed_xiao_nrf52840_sense -t upload

# Clean
pio run -e seeed_xiao_nrf52840_sense -t clean
```

### Dependencies

All declared in `platformio.ini`:

- Adafruit_NeoPixel
- Adafruit Bluefruit nRF52 BSP
- FortySevenEffects/MIDI Library
- Seeed Arduino LSM6DS3
- Adafruit LittleFS
- CD74HC4067

## Usage

1. Power on. The strip flashes the battery level briefly, then connects via
   BLE as `Glove`.
2. Pair from your host. The glove appears as a MIDI input.
3. Play by touching the thumb to a finger pad. The lit knuckle and the colour
   ripple confirm the note.
4. **Hold octave-up for 1 s** to enter the tap-tempo menu (transpose, tempo
   nudge, CC swap, flex range — all inside).
5. **Hold octave-down for 1 s** to enter the preset browser (load, save,
   quantize, brightness, tempo-bend).
6. **Hold the pinky palm for 1 s** to enter scale-edit mode (ch3/ch6 scroll
   through 20 scales; auto-exits after 3 s idle).
7. **Quick-tap the pinky palm** to cycle the note-spread (1–4).
8. **Double-tap the back of the hand** to flash the battery level.

Documented in detail: 

- User guide [glove_user_guide.html](https://htmlpreview.github.io/?https://github.com/sneak-thief/Sneaky-Gestures-V2/blob/main/documentation/glove_user_guide.html)
- LED display [LED_Mode_Reference.html](https://htmlpreview.github.io/?https://github.com/sneak-thief/Sneaky-Gestures-V2/blob/main/documentation/LED_Mode_Reference.html)

## Repository layout

```

├── src/
│   ├── main.cpp              firmware entry: input scan, MIDI, BLE, presets
│   ├── LedDisplay.{h,cpp}    LED rendering module
│   ├── DebugSerial.h         debug-print macros
│   └── TempoPitchShifter.{h,cpp}  inlined pitch-bend math library
├── hardware/
│   ├── Sneaky-Gestures-V2-Schematic_V1.0.png   Schematics V1 (png)
│   └── Sneaky-Gestures-V2-Schematic_V1.0.pdf   Schematics V1 (pdf)
├── platformio.ini
├── LED_Mode_Reference.html   illustrated LED reference
├── glove_layout.html         labelled glove diagram
├── glove_user_guide.html     user guide
└── README.md                 this file
```
