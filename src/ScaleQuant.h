/*****************************************************************************/
// Sneaky Gestures V2: MIDI BLE Gestural Glove
// https://github.com/sneak-thief/Sneaky-Gestures-V2
//
// Scale handler
//
// Credit: Audo Commander's ACSensorizer
// https://www.midibox.org/dokuwiki/acsensorizer_04
// 
// ScaleQuant.h
//
// Musical scale tables, scale-degree -> MIDI pitch mapping, the chromatic key
// spread, and the note/flex time-quantization grid intervals. Pure logic with
// no MIDI or hardware I/O; reads the shared musical/tempo state from
// GloveState.h.
#pragma once

#include <Arduino.h>

// Total number of scales (0 = no quantization, 1..19 = named scales).
#define SCALE_MAX 20

// Extract the ordered distinct semitone offsets for a scale into out[] (>=12).
// Returns the number of distinct degrees (N).
int getScaleDegrees(uint8_t scaleIdx, uint8_t out[12]);

// Convert a 0-based button position to a MIDI pitch (0..127), applying the
// current Scale, Spread, RootNote, octave, and rootTranspose (mode rotation).
// keyTranspose is applied by callers, not here.
int scaleDegreeToMidiPitch(int buttonPos);

// Rebuild the chromatic Keys[] table from a root/spread/offset (Scale 0 path).
void NoteSpread(int RootNote, int Spread, int RootNoteOffset);

// Time-grid quantization interval (ms) for the current note / flex modes, or 0
// when that mode is OFF. Derived from the current tempo.
unsigned long noteQuantizeIntervalMs();
unsigned long flexQuantizeIntervalMs();
