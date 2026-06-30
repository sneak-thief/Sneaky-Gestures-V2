/*****************************************************************************/
// Sneaky Gestures V2: MIDI BLE Gestural Glove
// https://github.com/sneak-thief/Sneaky-Gestures-V2
//
// Analog input filtering
//
// ---------------------------------------------------------------------------
//
// OneEuroFilter.h
//
// The "1€ filter" (Casiez, Roussel & Vogel, 2012): an adaptive low-pass filter
// designed specifically for noisy human-motion signals. It uses a low cutoff
// frequency when the input is slow or still -- so jitter is smoothed away while
// you hold a pose -- and automatically raises the cutoff as the input speeds up,
// so a deliberate gesture is tracked with little lag. This is a much better fit
// for hand-gesture MIDI control than a fixed exponential average, which forces a
// single jitter-vs-lag tradeoff.
//
// Tuning (both exposed via the constructor / setParams):
//   minCutoffHz : steady-state cutoff. LOWER = smoother & steadier when still
//                 (stronger jitter rejection), at the cost of a touch more lag.
//   beta        : speed coefficient. HIGHER = snappier response to fast,
//                 deliberate moves (less lag); too high lets jitter back in.
//   dCutoffHz   : cutoff for the internal speed estimate (1.0 Hz is fine).
#pragma once

#include <math.h>

class OneEuroFilter {
public:
  OneEuroFilter(float minCutoffHz = 1.0f, float beta = 0.02f, float dCutoffHz = 1.0f)
    : _minCutoff(minCutoffHz), _beta(beta), _dCutoff(dCutoffHz),
      _xPrev(0.0f), _dxPrev(0.0f), _tPrev(0.0f), _init(false) {}

  void setParams(float minCutoffHz, float beta, float dCutoffHz = 1.0f) {
    _minCutoff = minCutoffHz;
    _beta      = beta;
    _dCutoff   = dCutoffHz;
  }

  // Forget history; the next sample re-seeds the filter (no startup transient).
  void reset() { _init = false; }

  // Filter one sample. tSec is a monotonic timestamp in seconds (e.g.
  // millis()/1000.0f). Returns the smoothed value.
  float filter(float x, float tSec) {
    if (!_init) {
      _xPrev = x; _dxPrev = 0.0f; _tPrev = tSec; _init = true;
      return x;
    }

    float dt = tSec - _tPrev;
    if (dt <= 0.0f) dt = 1.0e-3f; // guard against zero/negative dt
    _tPrev = tSec;

    // Smoothed derivative (rate of change), itself low-passed at dCutoff.
    float dx  = (x - _xPrev) / dt;
    float aD  = alpha(_dCutoff, dt);
    float edx = aD * dx + (1.0f - aD) * _dxPrev;
    _dxPrev   = edx;

    // Speed-dependent cutoff: faster motion -> higher cutoff -> less smoothing.
    float cutoff = _minCutoff + _beta * fabsf(edx);
    float aX     = alpha(cutoff, dt);
    float xHat   = aX * x + (1.0f - aX) * _xPrev;
    _xPrev       = xHat;
    return xHat;
  }

private:
  // EMA smoothing factor for a given cutoff frequency and timestep.
  static float alpha(float cutoffHz, float dt) {
    float tau = 1.0f / (2.0f * 3.14159265358979f * cutoffHz);
    return 1.0f / (1.0f + tau / dt);
  }

  float _minCutoff, _beta, _dCutoff;
  float _xPrev, _dxPrev, _tPrev;
  bool  _init;
};
