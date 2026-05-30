#ifndef LED_DISPLAY_H
#define LED_DISPLAY_H

#include <stdint.h>
#include <Adafruit_NeoPixel.h>

// ---------------------------------------------------------------------------
// LED display / animation module.
//
// Extracted from main.cpp. Owns the NeoPixel strip rendering: per-finger note
// animations + ripple, and all modal indicator screens (octave, spread, scale,
// quantize, preset browser, brightness, tempo-bend, battery, tap tempo, flex
// bar, orange flash). The module reads a number of shared globals that remain
// owned by main.cpp; those are declared extern below.
// ---------------------------------------------------------------------------

// ----- shared animation types (defined here, used by main + module) -----
enum FingerId { FINGER_INDEX = 0, FINGER_MIDDLE = 1, FINGER_RING = 2, FINGER_PINKY = 3 };

enum AnimPhase {
  ANIM_IDLE = 0,
  ANIM_NOTE_ON,
  ANIM_HELD,
  ANIM_NOTE_OFF
};

struct FingerAnim {
  AnimPhase phase;
  unsigned long startMs;
  uint8_t sourceLed;
  uint8_t r, g, b;
  bool rippleActive;
  unsigned long rippleStartMs;
  uint8_t rippleR, rippleG, rippleB;
};

extern FingerAnim fingerAnims[4];

// Note/flex quantization modes (shared with main.cpp).
enum NoteQuantizeMode { NQ_OFF = 0, NQ_1_8 = 1, NQ_1_16 = 2, NQ_1_32 = 3, NQ_MODE_COUNT = 4 };
enum FlexQuantizeMode { FQ_OFF = 0, FQ_1_8 = 1, FQ_1_16 = 2, FQ_1_32 = 3, FQ_MODE_COUNT = 4 };

// ----- public API (called from main.cpp) -----
void TriggerFingerNoteOn(int channel);
void TriggerFingerNoteOff(int channel);
void TriggerOctaveDisplay();
void TriggerSpreadDisplay();
void TriggerScaleDisplay();
void TriggerBatteryDisplay();

void enterTapTempoMode();
void recordTapTempoPress();
void updateTapTempo();
void triggerOrangeFlash();

void UpdateFingerLeds();

#endif // LED_DISPLAY_H
