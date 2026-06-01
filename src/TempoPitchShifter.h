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


#ifndef TEMPO_PITCH_SHIFTER_H
#define TEMPO_PITCH_SHIFTER_H

#include <stdint.h>

// Computes the MIDI pitch-bend value needed to retune an audio sample
// recorded at one tempo so it plays in tune at a different tempo.
//
// Pitch shift in cents = 1200 * log2(targetTempo / originalTempo).
// This is mapped onto the 14-bit MIDI pitch-bend range (0..16383, center 8192)
// according to the configured bend range in semitones.
//
// No exceptions, no STL containers, no dynamic allocation: suitable for
// Arduino / Seeed XIAO nRF52840 and other embedded targets.

struct PitchBendResult {
    int16_t midiValue;   // 14-bit MIDI pitch bend, 0..16383, center 8192
    float   cents;       // required pitch shift in cents (+/-)
    float   semitones;   // required pitch shift in semitones (+/-)
    bool    outOfRange;  // true if shift exceeded bend range and was clamped
    bool    valid;       // false if inputs were invalid (tempos <= 0)
};

class TempoPitchShifter {
public:
    // bendRangeSemitones: total +/- range of the pitch wheel, in semitones.
    // Default is +/- 4 semitones.
    explicit TempoPitchShifter(float bendRangeSemitones = 4.0f);

    // Returns false and leaves the range unchanged if semitones <= 0.
    bool  setBendRange(float semitones);
    float bendRange() const;

    // Compute the pitch bend to retune a sample from originalTempo to
    // targetTempo. Check result.valid before using midiValue.
    PitchBendResult compute(float originalTempo, float targetTempo) const;

private:
    float bendRange_;
};

#endif // TEMPO_PITCH_SHIFTER_H
