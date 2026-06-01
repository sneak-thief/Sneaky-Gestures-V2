/*****************************************************************************/
// Sneaky Gestures V2: MIDI BLE Gestural Glove
// https://github.com/sneak-thief/Sneaky-Gestures-V2
//
// Scale handler
//
// ScaleQuant.cpp -- scale tables, pitch mapping, key spread, quantize grids.
#include "ScaleQuant.h"
#include "LedDisplay.h"    // NoteQuantizeMode / FlexQuantizeMode enums
#include "GloveState.h"    // shared musical + tempo state
#include "DebugSerial.h"

// Scale tables, scale-degree->MIDI pitch mapping, and the chromatic key
// spread now live in ScaleQuant.{h,cpp} (getScaleDegrees, scaleDegreeToMidiPitch,
// NoteSpread, SCALE_MAX, noteQuantizeIntervalMs, flexQuantizeIntervalMs).


// Chromatic->scale mapping: 20 scales x 12 semitones. Each entry is the
// semitone offset within the octave that the chromatic input maps to.
static const uint8_t scale_chromatic_map[SCALE_MAX][12] = {
  { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11}, // 0 not harmonized
  { 0, 0, 2, 4, 4, 5, 5, 7, 9, 9,11,11}, // 1 Major
  { 0, 0, 2, 3, 3, 5, 5, 7, 8, 8,10,10}, // 2 Natural Minor
  { 0, 0, 2, 3, 3, 5, 5, 7, 8, 8,11,11}, // 3 Harmonic Minor
  { 0, 0, 3, 3, 5, 5, 6, 7, 7,10,10,10}, // 4 Blues Minor
  { 0, 0, 3, 3, 4, 4, 7, 7, 9, 9,10,10}, // 5 Blues Major
  { 0, 2, 2, 3, 3, 5, 5, 7, 7, 9,10,10}, // 6 Dorian
  { 0, 0, 2, 2, 5, 5, 5, 7, 7, 7,10,10}, // 7 Japanese
  { 0, 1, 1, 1, 5, 5, 5, 7, 7, 7,10,10}, // 8 Japanese Diminished
  { 0, 2, 2, 3, 3, 3, 7, 7, 7, 9, 9, 9}, // 9 Kumoi
  { 0, 0, 2, 2, 4, 4, 6, 7, 7, 9, 9,11}, //10 Lydian
  { 0, 1, 1, 3, 3, 5, 6, 6, 8, 8,10,10}, //11 Locrian
  { 0, 0, 2, 3, 3, 6, 6, 7, 7, 9,10,10}, //12 Mi-Sheberach
  { 0, 0, 2, 2, 4, 5, 5, 7, 7, 9,10,10}, //13 Mixolydian
  { 0, 1, 1, 3, 3, 3, 7, 7, 8, 8, 8, 8}, //14 Pelog
  { 0, 0, 2, 2, 2, 5, 5, 7, 7, 7,10,10}, //15 Pentatonic Neutral
  { 0, 0, 3, 3, 5, 5, 6, 7, 7, 7,10,10}, //16 Pentatonic Blues
  { 0, 0, 2, 2, 4, 4, 7, 7, 7, 9, 9, 9}, //17 Pentatonic Major
  { 0, 0, 3, 3, 5, 5, 5, 7, 7, 7,10,10}, //18 Pentatonic Minor
  { 0, 1, 1, 3, 3, 5, 5, 7, 8, 8,10,10}  //19 Phrygian
};

int getScaleDegrees(uint8_t scaleIdx, uint8_t out[12])
{
  if (scaleIdx == 0 || scaleIdx >= SCALE_MAX) {
    for (int i = 0; i < 12; i++) out[i] = i;
    return 12;
  }
  int n = 0;
  uint8_t prev = 255;
  for (int i = 0; i < 12; i++) {
    uint8_t v = scale_chromatic_map[scaleIdx][i];
    if (v != prev) { out[n++] = v; prev = v; }
  }
  return n;
}

int scaleDegreeToMidiPitch(int buttonPos)
{
  int pitch;
  if (Scale == 0) {
    // No quantization: use existing Keys[] + spread path. Keys[] has 12 slots;
    // callers may probe higher buttonPos values (e.g. the flex candidate scan),
    // so clamp the index to stay in bounds.
    int idx = buttonPos;
    if (idx < 0) idx = 0;
    if (idx > 11) idx = 11;
    pitch = (int)Keys[idx] + OCTAVE_OFFSETS[octave];
  } else {
    uint8_t degrees[12];
    int n = getScaleDegrees((uint8_t)Scale, degrees);

    int spreadStep = (int)Spread;
    if (spreadStep < 1) spreadStep = 1;
    int degreeIndex = buttonPos * spreadStep;

    // rootTranspose = MODE rotation of the scale degrees (root pitch class fixed).
    int r = rootTranspose % n;
    if (r < 0) r += n;

    int totalIdx = degreeIndex + r;
    int rotIdx   = totalIdx % n;
    int octShift = (totalIdx / n) * 12;
    int rotatedDeg = (int)degrees[rotIdx] - (int)degrees[r] + octShift;
    if (rotatedDeg < 0) rotatedDeg += 12;

    int rootPitchClass = (int)(RootNote % 12);
    int rootOctave     = (int)(RootNote / 12) * 12;

    pitch = rootOctave + rootPitchClass + rotatedDeg + OCTAVE_OFFSETS[octave];
  }

  // (keyTranspose is applied by callers, not here, to avoid double-application.)
  if (pitch < 0) pitch = 0;
  if (pitch > 127) pitch = 127;
  return pitch;
}

void NoteSpread(int RootNote, int Spread, int RootNoteOffset)
{ // Change number of semitones between the keys, eg. +1, +2, +3 & +4
  for (int i = 1; i < 13; ++i) {
    Keys[i - 1] = 60 + ((i - 1) * Spread) + RootNoteOffset;
  }
  for (size_t i = 0; i < 12; ++i) {
    DBG_PRINT("Keys[");
    DBG_PRINT(i);
    DBG_PRINT("] = ");
    DBG_PRINTLN(Keys[i]);
  }
}

unsigned long noteQuantizeIntervalMs()
{
  if (noteQuantizeMode == NQ_OFF) return 0;
  unsigned long beatMs = 60000UL / (unsigned long)tempo;
  if (noteQuantizeMode == NQ_1_8)  return beatMs / 2UL;
  if (noteQuantizeMode == NQ_1_16) return beatMs / 4UL;
  if (noteQuantizeMode == NQ_1_32) return beatMs / 8UL;
  return 0;
}

unsigned long flexQuantizeIntervalMs()
{
  if (flexQuantizeMode == FQ_OFF) return 0;
  unsigned long beatMs = 60000UL / (unsigned long)tempo;
  if (flexQuantizeMode == FQ_1_8)  return beatMs / 2UL;
  if (flexQuantizeMode == FQ_1_16) return beatMs / 4UL;
  if (flexQuantizeMode == FQ_1_32) return beatMs / 8UL;
  return 0;
}
