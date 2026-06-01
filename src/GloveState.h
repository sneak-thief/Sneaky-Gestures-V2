/*****************************************************************************/
// Sneaky Gestures V2: MIDI BLE Gestural Glove
// https://github.com/sneak-thief/Sneaky-Gestures-V2
//
// Glove state handler
//
// GloveState.h
//
// Shared global state for the glove firmware. The variables themselves are
// DEFINED in main.cpp (which owns the input scan / loop that mutates them); the
// extracted modules (ScaleQuant, TempoControl, Presets) read/write them through
// these extern declarations. Keeping the definitions in one translation unit
// avoids multiple-definition linker errors while still letting the musical /
// tempo / preset logic live in their own files.
#pragma once

#include <Arduino.h>
#include "TempoPitchShifter.h"

// --- Musical parameters (defined in main.cpp) -------------------------------
extern unsigned int Keys[12];
extern unsigned int RootNote;
extern unsigned int Spread;
extern signed int   RootNoteOffset;
extern unsigned int Scale;

extern const signed int OCTAVE_OFFSETS[7];
extern const int OCTAVE_DEFAULT;
extern const int OCTAVE_MAX_INDEX;
extern int octave;

extern const int KEY_TRANSPOSE_MIN;
extern const int KEY_TRANSPOSE_MAX;
extern int keyTranspose;

extern const int ROOT_TRANSPOSE_MIN;
extern const int ROOT_TRANSPOSE_MAX;
extern int rootTranspose;

// --- Flex note range (defined in main.cpp) ----------------------------------
extern int minNote;
extern int maxNote;

// --- Quantization mode + grid timers (defined in main.cpp) ------------------
extern int noteQuantizeMode;             // NoteQuantizeMode (see LedDisplay.h)
extern int flexQuantizeMode;             // FlexQuantizeMode (see LedDisplay.h)
extern unsigned long noteQuantizeNextGridMs;
extern unsigned long flexQuantizeMs;
extern unsigned long flexNextGridMs;

// --- Tempo + clock sync state (defined in main.cpp) -------------------------
extern const int TEMPO_MIN;
extern const int TEMPO_MAX;
extern int tempo;

extern const float CLOCK_EMA_ALPHA;
extern const unsigned long CLOCK_TIMEOUT_MS;
extern const int CLOCK_MIN_SAMPLES;
extern const int CLOCK_PPQN;
extern unsigned long lastClockMs;
extern float clockEmaIntervalMs;
extern int  clockSampleCount;
extern bool externalClockActive;

// --- Tempo-tracking pitch bend (defined in main.cpp) ------------------------
extern bool tempoBendEnabled;
extern int  projectTempo;
extern const unsigned long TEMPO_BEND_INTERVAL_MS;
extern unsigned long lastTempoBendMs;
extern int  lastSentBendTempo;
extern TempoPitchShifter tempoShifter;

// --- Patch save/load confirmation flash (defined in main.cpp) ---------------
extern bool patchConfirmActive;
extern unsigned long patchConfirmStartMs;
extern int  patchConfirmKind;
