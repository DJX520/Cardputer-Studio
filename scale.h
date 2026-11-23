// scale.h — musical scales + note names (shared by synth, chord builder, quantizer)
#pragma once

extern const char* const kNoteNames[12];   // "C","C#",... "B"
extern const char* const kScaleNames[13];  // "CHR","MAJ",... "WHL"

int    scaleCount();
unsigned scaleMask(int scale);            // 12-bit semitone mask above root
int    scaleSize(int scale);              // notes per octave
int    scaleDegree(int scale, int k);     // semitones above root of degree k (k may be negative)
int    midiFromDegree(int root, int scale, int deg);   // MIDI note of a scale degree
int    quantizeMidi(int midi, int scale, int root);    // nearest in-scale MIDI note
bool   scaleIsSevenNote(int scale);       // 7-note scales support diatonic triads
