/*****************************************************************************/
// Sneaky Gestures V2: MIDI BLE Gestural Glove
// https://github.com/sneak-thief/Sneaky-Gestures-V2
//
// Tempo bpm handler 
//
// ---------------------------------------------------------------------------


// TempoControl.cpp -- global tempo set + dependent grid derivation.
#include "TempoControl.h"
#include "ScaleQuant.h"    // flexQuantizeIntervalMs()
#include "GloveState.h"    // tempo, TEMPO_MIN/MAX, flexQuantizeMs
#include "DebugSerial.h"

void setTempo(int bpm)
{
  if (bpm < TEMPO_MIN) bpm = TEMPO_MIN;
  if (bpm > TEMPO_MAX) bpm = TEMPO_MAX;
  tempo = bpm;
  // Flex grid interval derives from the flex-quantize mode (independent of the
  // note-quantize mode). When flex quantize is OFF the interval is 0; keep a
  // nonzero fallback in flexQuantizeMs so the grid-advance math never divides
  // by zero (the OFF path in FlexNoteFlush bypasses the grid anyway).
  unsigned long fi = flexQuantizeIntervalMs();
  flexQuantizeMs = (fi > 0) ? fi : (60000UL / (unsigned long)bpm / 4UL);
  DBG_PRINT("Tempo set to ");
  DBG_PRINT(tempo);
  DBG_PRINT(" BPM, flex grid = ");
  DBG_PRINT(flexQuantizeMs);
  DBG_PRINTLN(" ms");
}
