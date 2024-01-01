// ACHarmonizer.cpp

#include "ACHarmonizer.h"

// const unsigned char	noteNames[2][12] = {
// { 'C', 'C', 'D', 'D', 'E', 'F', 'F', 'G', 'G', 'A', 'A', 'B' },
// { ' ', '#', ' ', '#', ' ', ' ', '#', ' ', '#', ' ', '#', ' ' }
// };

const unsigned char ACHarmonizer::scale_set[SCALE_MAX][12] = {

//	C	C#	D	D#	E	F	F#	G	G#	A	A#	B
{	0,	1,	2,	3,	4,	5,	6,	7,	8,	9,	10,	11	},	// not harmonized
{	0,	0,	2,	4,	4,	5,	5,	7,	9,	9,	11,	11	},	// Major
{	0,	0,	2,	3,	3,	5,	5,	7,	8,	8,	10,	10	},	// Natural Minor = Aeolian
{	0,	0,	2,	3,	3,	5,	5,	7,	8,	8,	11,	11	},	// Harmonic Minor
{	0,	0,	3,	3,	5,	5,	6,	7,	7,	10,	10,	10	},	// Blues Minor
{	0,	0,	3,	3,	4,	4,	7,	7,	9,	9,	10,	10	},	// Blues Major
{	0,	2,	2,	3,	3,	5,	5,	7,	7,	9,	10,	10	},	// Dorian
{	0,	0,	2,	2,	5,	5,	5,	7,	7,	7,	10,	10	},	// Japanese
{	0,	1,	1,	1,	5,	5,	5,	7,	7,	7,	10,	10	},	// Japanese Diminished
{	0,	2,	2,	3,	3,	3,	7,	7,	7,	9,	9,	9	},	// Kumoi
{	0,	0,	2,	2,	4,	4,	6,	7,	7,	9,	9,	11	},	// Lydian
{	0,	1,	1,	3,	3,	5,	6,	6,	8,	8,	10,	10	},	// Locrian
{	0,	0,	2,	3,	3,	6,	6,	7,	7,	9,	10,	10	},	// Mi-Sheberach (Jewish)
{	0,	0,	2,	2,	4,	5,	5,	7,	7,	9,	10,	10	},	// Mixolydian
{	0,	1,	1,	3,	3,	3,	7,	7,	8,	8,	8,	8	},	// Pelog
{	0,	0,	2,	2,	2,	5,	5,	7,	7,	7,	10,	10	},	// Pentatonic Neutral
{	0,	0,	3,	3,	5,	5,	6,	7,	7,	7,	10,	10	},	// Pentatonic Blues
{	0,	0,	2,	2,	4,	4,	7,	7,	7,	9,	9,	9	},	// Pentatonic Major
{	0,	0,	3,	3,	5,	5,	5,	7,	7,	7,	10,	10	},	// Pentatonic Minor
{	0,	1,	1,	3,	3,	5,	5,	7,	8,	8,	10,	10	}	// Phrygian
};


ACHarmonizer::ACHarmonizer() {
    harmonizer_base = BASE_DEFAULT;
    harmonizer_scale = SCALE_DEFAULT;
    harmonizer_listen = 0;
}

void ACHarmonizer::begin() {
    // Initialization code, if any

}

void ACHarmonizer::setBase(unsigned char value) {
    for (unsigned char c = 0; c < 127; c++) {
        if (value < 12) {
            harmonizer_base = value;
            return;
        } else {
            value = value - 12;
        }
    }
}


void ACHarmonizer::setScale(unsigned char value) {
    // select current scale
    if (value > SCALE_MAX) {
        // switch to next scale
        toggleScale(1);
    } else {
        harmonizer_scale = value;
    }
}

void ACHarmonizer::toggleScale(unsigned char direction) {
    // 0:down, 1:up
    if (direction) {
        // switch to next scale
        harmonizer_scale++;
        if (harmonizer_scale > (SCALE_MAX - 1)) {
            harmonizer_scale = 0;
        }
    } else {
        // switch to previous scale
        if (harmonizer_scale == 0) {
            harmonizer_scale = (SCALE_MAX - 1);
        } else {
            harmonizer_scale--;
        }
    }
}

unsigned char ACHarmonizer::harmonize(unsigned char value, unsigned char scale) {
    // 0..127 in, returns harmonized value
    unsigned char c;
    unsigned char note_interval = 0;
    unsigned char note_num = value - harmonizer_base;
    unsigned char harmonized = value;
    harmonizer_scale = scale;
    // make life easier
    if (harmonizer_scale == 0)
        return value;

    // get note number from value
    for (c = 0; c <= value; c++) {
        if (note_num < 12) {
            // got value! now harmonize:
            harmonized = scale_set[harmonizer_scale][note_num];
            harmonized = harmonized + note_interval;
            break;
        } else {
            note_num = note_num - 12;
            note_interval = note_interval + 12;
        }
    }

    return harmonized + harmonizer_base;
}
