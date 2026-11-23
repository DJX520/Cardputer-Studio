// scale.cpp — musical scales and note names.
#include "scale.h"

const char* const kNoteNames[12] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
const char* const kScaleNames[13] = {"CHR","MAJ","MIN","DOR","PHR","LYD","MIX","LOC","HMN","PN+","PN-","BLU","WHL"};

namespace {
// 12-bit masks, bit 0 = root. Same order as kScaleNames.
const unsigned kMasks[13] = {
    0xFFF,                              // 0 chromatic
    0b101010110101,                     // 1 major
    0b010110101101,                     // 2 natural minor
    0b011010101101,                     // 3 dorian
    0b010110101011,                     // 4 phrygian
    0b101011010101,                     // 5 lydian
    0b011010110101,                     // 6 mixolydian
    0b010101101011,                     // 7 locrian
    0b100110101101,                     // 8 harmonic minor
    0b001010010101,                     // 9 major pentatonic
    0b010010101001,                     // 10 minor pentatonic
    0b010011101001,                     // 11 blues
    0b010101010101,                     // 12 whole tone
};
}

int scaleCount() { return 13; }
unsigned scaleMask(int s) { return kMasks[(s < 0 || s >= 13) ? 0 : s]; }

int scaleSize(int s) {
    unsigned m = scaleMask(s);
    int n = 0;
    for (int i = 0; i < 12; i++) n += (m >> i) & 1;
    return n;
}

int scaleDegree(int s, int k) {
    const int n = scaleSize(s);
    if (n <= 0) return 0;
    int oct = k >= 0 ? k / n : -((-k + n - 1) / n);
    int d = k - oct * n;
    unsigned m = scaleMask(s);
    for (int i = 0; i < 12; i++) {
        if ((m >> i) & 1) {
            if (d == 0) return oct * 12 + i;
            d--;
        }
    }
    return oct * 12;
}

int midiFromDegree(int root, int scale, int deg) {
    return root + scaleDegree(scale, deg);
}

int quantizeMidi(int midi, int scale, int root) {
    unsigned m = scaleMask(scale);
    int pc = ((midi - root) % 12 + 12) % 12;
    if ((m >> pc) & 1) return midi;          // already in scale
    // walk outward for the nearest in-scale semitone (ties go up)
    for (int d = 1; d <= 12; d++) {
        if (((m >> ((pc + d) % 12)) & 1)) return midi + d;
        if (((m >> ((pc - d + 12) % 12)) & 1)) return midi - d;
    }
    return midi;
}

bool scaleIsSevenNote(int s) { return scaleSize(s) == 7; }
