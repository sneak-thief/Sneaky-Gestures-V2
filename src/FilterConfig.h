/*****************************************************************************/
// Sneaky Gestures V2: MIDI BLE Gestural Glove
// https://github.com/sneak-thief/Sneaky-Gestures-V2
//
// Analog input filtering
//
// ---------------------------------------------------------------------------
//
// FilterConfig.h
//
// Tunable One Euro low-pass parameters for each smoothed input, so the
// accelerometer X/Y CCs, the thumb-FSR aftertouch (AT), and the index flex
// sensor can each be adjusted independently. The variables are DEFINED in
// main.cpp and applied to the filters every sample, so editing them (here's
// where to look for the defaults) -- or changing them at runtime -- takes
// effect immediately.
//
//   *MinCutoff (Hz): the steady-state cutoff. LOWER = smoother and steadier
//                    when the input is still (stronger jitter rejection), at the
//                    cost of slightly more lag.
//   *Beta          : the speed coefficient. HIGHER = snappier response to fast,
//                    deliberate moves (less lag); too high lets jitter back in.
#pragma once

// Accelerometer X/Y CCs (shared by both axes).
extern float accelFilterMinCutoff;
extern float accelFilterBeta;

// Thumb-FSR channel aftertouch.
extern float atFilterMinCutoff;
extern float atFilterBeta;

// Index flex sensor (kept mild -- light smoothing only).
extern float flexFilterMinCutoff;
extern float flexFilterBeta;
