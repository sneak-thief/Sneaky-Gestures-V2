/*****************************************************************************/
// Sneaky Gestures V2: MIDI BLE Gestural Glove
// https://github.com/sneak-thief/Sneaky-Gestures-V2
//
// TempoControl.h
//
// Global tempo handling: clamp/set the BPM and re-derive the dependent flex
// quantization grid. The MIDI-clock EMA sync lives in midiRead() and the
// tempo-tracking pitch bend (UpdateTempoBend) lives in main.cpp, since both are
// tied to the MIDI instance; this module owns the pure tempo math.
#pragma once

// Set the global tempo (clamped to [TEMPO_MIN..TEMPO_MAX]) and re-derive the
// flex-quantize grid interval. Call any time the tempo changes.
void setTempo(int bpm);
