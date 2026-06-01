/*****************************************************************************/
// Sneaky Gestures V2: MIDI BLE Gestural Glove
// https://github.com/sneak-thief/Sneaky-Gestures-V2
//
// Tempo-to-pitch handler
//
// For syncing the pitch to audio sources whose pitch 
// that changes when the tempo is changed (eg. vinyl, CDJs)
// When a preset is set to a specific tempo, read the incoming MIDI tempo
// and send a MIDI pitch bend command to match the instrument's pitch
// to the accompanying audio track. Set the instrument's pitch bend range to
// +/4 semitones
// ---------------------------------------------------------------------------


#include "TempoPitchShifter.h"

#include <math.h>   // log2, lround

TempoPitchShifter::TempoPitchShifter(float bendRangeSemitones)
    : bendRange_(bendRangeSemitones > 0.0f ? bendRangeSemitones : 4.0f)
{
}

bool TempoPitchShifter::setBendRange(float semitones)
{
    if (semitones <= 0.0f) {
        return false;
    }
    bendRange_ = semitones;
    return true;
}

float TempoPitchShifter::bendRange() const
{
    return bendRange_;
}

PitchBendResult TempoPitchShifter::compute(float originalTempo, float targetTempo) const
{
    PitchBendResult r;
    r.midiValue  = 8192;
    r.cents      = 0.0f;
    r.semitones  = 0.0f;
    r.outOfRange = false;
    r.valid      = false;

    if (originalTempo <= 0.0f || targetTempo <= 0.0f) {
        return r;  // valid stays false
    }

    // Use double for the log/ratio math; this is a one-shot calculation,
    // so the software-float cost on the nRF52840 is negligible.
    const double cents     = 1200.0 * log2((double)targetTempo / (double)originalTempo);
    const double semitones = cents / 100.0;

    // Fraction of the full bend range this shift represents (-1..+1 inside range).
    // bendRange_ is in semitones; convert to cents to keep units consistent.
    const double fraction = cents / ((double)bendRange_ * 100.0);

    // 8192 counts span the full bend range in each direction.
    double midiF = 8192.0 + fraction * 8192.0;

    r.cents     = (float)cents;
    r.semitones = (float)semitones;
    r.outOfRange = (midiF < 0.0 || midiF > 16383.0);

    long midi = lround(midiF);
    if (midi < 0)        midi = 0;
    else if (midi > 16383) midi = 16383;

    r.midiValue = (int16_t)midi;
    r.valid     = true;
    return r;
}
