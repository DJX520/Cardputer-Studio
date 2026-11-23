// synth_app.cpp — performance synth: note mapping (scale-locked "strings"),
// glide, chord mode, harmony, arpeggiator, drones, metronome, tap tempo, a note
// looper, tilt, ten sound slots with randomize/mutate/undo and SD save/load.
#include "synth_app.h"
#include "config.h"
#include "theme.h"
#include "keyboard.h"
#include "scale.h"
#include "synth_engine.h"
#include "settings.h"
#include "sd_util.h"
#include "listen.h"
#include "looper.h"
#include <M5Unified.h>
#include <math.h>

// ---- note grid ------------------------------------------------------------
// Four rows tuned in fourths, low to high. Bottom row = drone latch; the top
// three rows are the solo "strings".
static const char* kStr[4] = {"zxcvbnm,./", "asdfghjkl;", "qwertyuiop", "1234567890"};
static const int   kOffset[4] = {0, 5, 10, 15};

// ---- state ----------------------------------------------------------------
static Patch  gSlots[kMaxSlots];
static int    gSlot = 0;
static Patch  gLive;
static Patch  gLastSound;      // tilt-morph target ("the sound you were just on")
static bool   gDirty = false;

static Patch  gUndo[8];
static int    gUndoN = 0;

static int    gRoot = 9, gScale = 10, gOctave = 0;   // A minor pentatonic
static bool   gChordMode = false, gHarmony = false, gDrumMode = false;
static int    gArpMode = 0;      // 0 off, 1 up, 2 down, 3 up-down
static int    gArpDiv = 2;       // 0=1/4, 1=1/8, 2=1/8T, 3=1/16
static bool   gArpSpan2 = false;
static float  gTempo = 100.0f;
static bool   gMetro = false;
static float  gMaster = 0.7f;
static bool   gTiltOn = false;
static int    gTiltMode = 0;     // 0 morph(cutoff), 1 cutoff, 2 vibrato, 3 volume
static float  gBend = 0.0f;
static int    gChordQual = 0;    // chromatic chord: 0 maj 1 min 2 maj7 3 min7 4 dom7 5 dim 6 aug
static int    gEditParam = -1;   // fn+1..8 -> parameter index, -1 none

// held solo keys + latched drones
static bool gHeld[4][10];
static bool gDrone[10];

// chord auto-progression (bottom row in "progression" jam mode)
static int gJamMode = 0;        // 0 = drone latch, 1 = chord progression
static int gProg[8][3];         // up to 8 steps: root, 3rd, 5th
static int gProgN = 0, gProgIdx = 0;
static int gBeatCount = 0;      // beats since last chord change

// arp
static int  gArpNotes[16]; static int gArpN = 0; static int gArpIdx = -1;

static int    gScreen = 0;       // 0 play, 1 fn picker, 2 menu
static int    gMenuSel = 0;
static uint32_t gLastDraw = 0, gLastMetro = 0, gLastArp = 0, gLastTap = 0;
static float  gTapSum = 0; static int gTapN = 0;

// fn+k hold -> LISTEN
static uint32_t gFnKDown = 0; static bool gListenDone = false;

// ---- helpers --------------------------------------------------------------
static bool keyboardChromatic() { return keyHeld(K_SHIFT); }
static void pushPatchOnce(const Patch& p);
static void doListen();
static int baseMidi() { return gRoot + 36 + 12 * gOctave; }

// number of scale degrees that span a perfect 4th (5 semitones); rows are
// spaced this many degrees so every key is always in-scale.
static int rowDeg() {
    const int n = scaleSize(gScale);
    if (gScale == 0) return 5;
    for (int k = 1; k < n; k++) {
        const int iv = ((scaleDegree(gScale, k) - scaleDegree(gScale, 0)) % 12 + 12) % 12;
        if (iv >= 5) return k;
    }
    return 1;
}

static int noteAt(int row, int col) {
    const int b = baseMidi();
    if (keyboardChromatic()) return b + kOffset[row] + col;
    return b + scaleDegree(gScale, row * rowDeg() + col);
}

static void pushLive() {
    Synth::get().setPatch(gLive);
    Synth::get().setMaster(gMaster);
}
static void pushScale() {
    // scale/root are applied in noteAt(); nothing to push to the engine
}

static int chordQualInterval(int q, int idx) {  // idx 0..3
    static const int8_t T[7][4] = {
        {0,4,7,-1},{0,3,7,-1},{0,4,7,11},{0,3,7,10},{0,4,7,10},{0,3,6,-1},{0,4,8,-1}
    };
    return T[q][idx];
}

static int buildChord(int row, int col, int* notes) {
    // returns count of EXTRA notes (3rd/5th/7th); root is handled by caller.
    const int base = noteAt(row, col);
    const int deg = row * rowDeg() + col;   // scale degree of the root
    int n = 0;
    if (gChordMode) {
        if (gScale == 0) {   // chromatic: pick chord quality
            for (int i = 1; i < 4; i++) { int iv = chordQualInterval(gChordQual, i); if (iv < 0) break; notes[n++] = base + iv; }
        } else if (scaleIsSevenNote(gScale)) {
            notes[n++] = base + scaleDegree(gScale, deg + 2) - scaleDegree(gScale, deg);
            notes[n++] = base + scaleDegree(gScale, deg + 4) - scaleDegree(gScale, deg);
        }
    } else if (gHarmony) {
        notes[n++] = base + scaleDegree(gScale, deg + 2) - scaleDegree(gScale, deg);
    }
    return n;
}

static void rebuildArpSet() {
    // release the currently-sounding arp note before rebuilding the set
    if (gArpIdx >= 0 && gArpIdx < gArpN) Synth::get().noteOffString(-1, gArpNotes[gArpIdx]);
    gArpN = 0; gArpIdx = -1;
    // prefer the chord tones currently held, else drones
    for (int row = 0; row < 4; row++) for (int c = 0; c < 10; c++) {
        if (gHeld[row][c]) {
            gArpNotes[gArpN++] = noteAt(row, c);
            if (gArpN >= 16) return;
        }
    }
    if (gArpN == 0) for (int c = 0; c < 10; c++) if (gDrone[c]) gArpNotes[gArpN++] = noteAt(0, c);
    // arp span: add the octave-up layer
    if (gArpSpan2) {
        const int base = gArpN;
        for (int i = 0; i < base && gArpN < 16; i++) gArpNotes[gArpN++] = gArpNotes[i] + 12;
    }
}

static void pressKey(int row, int col) {
    if (gHeld[row][col]) return;
    gHeld[row][col] = true;
    const int midi = noteAt(row, col);
    if (row == 0) return;  // bottom row: drones handled on tap (edge)
    int extra[4]; int ne = buildChord(row, col, extra);
    Synth& s = Synth::get();
    s.noteOnString(row, midi, 0.9f);
    for (int i = 0; i < ne; i++) s.noteOnString(-1, extra[i], 0.7f);
    rebuildArpSet();
}

static void releaseKey(int row, int col) {
    if (!gHeld[row][col]) return;
    gHeld[row][col] = false;
    const int midi = noteAt(row, col);
    if (row == 0) return;
    int extra[4]; int ne = buildChord(row, col, extra);
    Synth& s = Synth::get();
    s.noteOffString(row, midi);
    for (int i = 0; i < ne; i++) s.noteOffString(-1, extra[i]);
    rebuildArpSet();
}

static void toggleDrone(int col) {
    gDrone[col] = !gDrone[col];
    if (gDrone[col]) Synth::get().noteOnString(-1, noteAt(0, col), 0.75f);
    else Synth::get().noteOffString(-1, noteAt(0, col));
    rebuildArpSet();
}

static void drumTap(int col) {
    // bottom-row pads: 0-2 kick, 3-5 snare, 6-7 hat, 8-9 clap
    const int t = col < 3 ? 0 : col < 6 ? 1 : col < 8 ? 2 : 3;
    Synth::get().drum(t, 0.9f);
}

// ---- chord auto-progression ----------------------------------------------
static int gProgSounding[3]; static int gProgSoundingN = 0;
static void progSound(int idx);

static int progChordNotes(int col, int* out) {
    const int root = noteAt(0, col);
    out[0] = root;
    if (gScale != 0 && scaleIsSevenNote(gScale)) {
        const int deg = col;
        out[1] = root + scaleDegree(gScale, deg + 2) - scaleDegree(gScale, deg);
        out[2] = root + scaleDegree(gScale, deg + 4) - scaleDegree(gScale, deg);
        return 3;
    }
    out[1] = root + 7;   // power chord (5th) when the scale has no diatonic triads
    return 2;
}

static void progAddChord(int col) {
    if (gProgN >= 8) return;
    int c[3]; int n = progChordNotes(col, c);
    gProg[gProgN][0] = c[0]; gProg[gProgN][1] = c[1]; gProg[gProgN][2] = (n > 2) ? c[2] : c[1];
    gProgN++;
    if (gProgN == 1) { gProgIdx = 0; gBeatCount = 0; progSound(0); }   // first chord sounds now
}

static void progSound(int idx) {
    Synth& s = Synth::get();
    for (int i = 0; i < gProgSoundingN; i++) s.noteOffString(-1, gProgSounding[i]);
    gProgSounding[0] = gProg[idx][0]; gProgSounding[1] = gProg[idx][1]; gProgSounding[2] = gProg[idx][2];
    gProgSoundingN = 3;
    s.noteOnString(-1, gProg[idx][0], 0.7f);
    s.noteOnString(-1, gProg[idx][1], 0.6f);
    s.noteOnString(-1, gProg[idx][2], 0.6f);
}

static void progAdvance() {
    if (gProgN == 0) return;
    gProgIdx = (gProgIdx + 1) % gProgN;
    progSound(gProgIdx);
}

static void progClear() {
    Synth& s = Synth::get();
    for (int i = 0; i < gProgSoundingN; i++) s.noteOffString(-1, gProgSounding[i]);
    gProgN = 0; gProgIdx = 0; gProgSoundingN = 0; gBeatCount = 0;
}

static void tapTempo() {
    const uint32_t now = millis();
    if (now - gLastTap > 2000) { gTapN = 0; gTapSum = 0; }
    if (gTapN > 0) {
        const float d = (float)(now - gLastTap) / 1000.0f;
        gTapSum += 60000.0f / d;
        gTempo = gTapSum / (float)gTapN;
        if (gTempo < 40) gTempo = 40; if (gTempo > 240) gTempo = 240;
    }
    gTapN++; gLastTap = now;
}

// ---- randomize / mutate ---------------------------------------------------
static Patch lerpPatch(const Patch& a, const Patch& b, float t) {
    if (t < 0) t = 0; if (t > 1) t = 1;
    const float q = 1.f - t;
    Patch p = a;
    p.cutoff = a.cutoff*q + b.cutoff*t;
    p.reso   = a.reso*q   + b.reso*t;
    p.glide  = a.glide*q  + b.glide*t;
    p.atk = a.atk*q + b.atk*t;  p.dec = a.dec*q + b.dec*t;  p.sus = a.sus*q + b.sus*t;  p.rel = a.rel*q + b.rel*t;
    p.fenvAtk = a.fenvAtk*q + b.fenvAtk*t;  p.fenvDec = a.fenvDec*q + b.fenvDec*t;  p.fenvDepth = a.fenvDepth*q + b.fenvDepth*t;
    p.sub = a.sub*q + b.sub*t;  p.noise = a.noise*q + b.noise*t;  p.drive = a.drive*q + b.drive*t;  p.vibrato = a.vibrato*q + b.vibrato*t;
    p.chorus = a.chorus*q + b.chorus*t;  p.delaySend = a.delaySend*q + b.delaySend*t;
    p.delayTime = a.delayTime*q + b.delayTime*t;  p.delayFb = a.delayFb*q + b.delayFb*t;
    p.reverbSend = a.reverbSend*q + b.reverbSend*t;  p.reverbSize = a.reverbSize*q + b.reverbSize*t;
    p.colorAmt = a.colorAmt*q + b.colorAmt*t;  p.fatDet = a.fatDet*q + b.fatDet*t;
    p.lfo1Rate = a.lfo1Rate*q + b.lfo1Rate*t;  p.lfo1Amt = a.lfo1Amt*q + b.lfo1Amt*t;
    p.lfo2Rate = a.lfo2Rate*q + b.lfo2Rate*t;  p.lfo2Amt = a.lfo2Amt*q + b.lfo2Amt*t;
    p.modAtk = a.modAtk*q + b.modAtk*t;  p.modDec = a.modDec*q + b.modDec*t;  p.modAmt = a.modAmt*q + b.modAmt*t;
    p.drift = a.drift*q + b.drift*t;
    // discrete fields switch at the midpoint
    if (t >= 0.5f) {
        p.wave = b.wave; p.color = b.color; p.fmode = b.fmode;
        p.lfo1Shape = b.lfo1Shape; p.lfo2Shape = b.lfo2Shape;
        p.lfo1Dest = b.lfo1Dest; p.lfo2Dest = b.lfo2Dest;
    }
    return p;
}

static void checkpoint() { if (gUndoN < 8) gUndo[gUndoN++] = gLive; gLastSound = gLive; }

static const char* kChars[14] = {"PLUCK","BELL","PAD","BASS","ACID","LEAD","BRASS","CHIP","SWARM","CHORD","VOWEL","GLASS","GRAIN","WAVE"};
static const char* kNouns[12] = {"haze","frost","ember","dune","tide","comet","drift","glimmer","shard","wisp","bloom","echo"};

static float rnd(float a, float b) { return a + (float)random(10000) / 10000.0f * (b - a); }

static void randomize(int family) {
    checkpoint();
    gLive = Patch();
    const int f = family >= 0 ? family : random(14);
    switch (f) {
        case 0: // pluck (Karplus-Strong string)
            gLive.wave = W_STRING; gLive.color = C_OFF; gLive.colorAmt = rnd(0.2f, 0.8f);
            gLive.reverbSend = rnd(0.1f, 0.4f);
            break;
        case 1: // bell (modal resonator / wave terrain)
            gLive.wave = (random(3) ? W_MODAL : W_WAVE_TERRAIN); gLive.color = C_OFF; gLive.colorAmt = rnd(0.4f, 0.9f);
            gLive.reverbSend = rnd(0.3f, 0.6f); gLive.reverbSize = rnd(0.4f, 0.8f);
            break;
        case 2: // pad
            gLive.wave = (random(4) == 0) ? W_STRING_MACHINE : (Wave)random(0, 2);
            gLive.atk = rnd(0.4f, 1.5f); gLive.dec = 0.5f; gLive.sus = 0.8f; gLive.rel = rnd(0.8f, 2.0f);
            gLive.cutoff = rnd(1200, 3500); gLive.reso = rnd(0.1f, 0.3f); gLive.chorus = rnd(0.3f, 0.7f); gLive.reverbSend = rnd(0.3f, 0.6f); gLive.reverbSize = rnd(0.5f, 0.9f);
            gLive.lfo1Shape = L_SINE; gLive.lfo1Rate = rnd(0.1f, 0.6f); gLive.lfo1Dest = D_CUTOFF; gLive.lfo1Amt = rnd(0.1f, 0.4f);
            break;
        case 3: // bass
            gLive.wave = W_SAW; gLive.sub = rnd(0.5f, 1.0f); gLive.cutoff = rnd(300, 1200); gLive.reso = rnd(0.2f, 0.7f);
            gLive.atk = 0.003f; gLive.dec = 0.2f; gLive.sus = 0.8f; gLive.rel = 0.15f; gLive.drive = rnd(1.0f, 3.0f);
            break;
        case 4: // acid
            gLive.wave = W_SAW; gLive.color = C_OFF; gLive.cutoff = rnd(400, 1500); gLive.reso = rnd(0.6f, 0.95f);
            gLive.fenvDepth = rnd(0.5f, 1.0f); gLive.fenvDec = rnd(0.1f, 0.3f); gLive.drive = rnd(1.5f, 4.0f);
            gLive.atk = 0.002f; gLive.dec = 0.15f; gLive.sus = 0.6f; gLive.rel = 0.1f;
            break;
        case 5: // lead
            gLive.wave = (Wave)random(2, 6); gLive.glide = rnd(0.05f, 0.2f); gLive.cutoff = rnd(2500, 7000); gLive.reso = rnd(0.2f, 0.6f);
            gLive.vibrato = rnd(0.2f, 0.6f); gLive.drive = rnd(0.8f, 2.0f); gLive.delaySend = rnd(0.2f, 0.5f); gLive.delayTime = rnd(0.2f, 0.4f);
            break;
        case 6: // brass
            gLive.wave = W_SAW; gLive.color = C_FM; gLive.colorAmt = rnd(0.1f, 0.4f); gLive.atk = rnd(0.1f, 0.3f); gLive.dec = 0.2f; gLive.sus = 0.8f; gLive.rel = 0.2f;
            gLive.cutoff = rnd(2000, 5000); gLive.fenvDepth = rnd(0.3f, 0.7f); gLive.chorus = rnd(0.2f, 0.4f);
            break;
        case 7: // chip
            gLive.wave = (random(2) ? W_SQUARE : W_CHIPTUNE); gLive.color = C_OFF; gLive.colorAmt = rnd(0.0f, 1.0f); gLive.cutoff = rnd(2000, 8000); gLive.reso = 0.2f;
            gLive.atk = 0.001f; gLive.dec = 0.1f; gLive.sus = 0.5f; gLive.rel = 0.05f; gLive.noise = rnd(0.05f, 0.2f);
            break;
        case 8: // swarm / supersaw
            gLive.wave = (random(2) ? W_SWARM : W_FAT); gLive.color = C_OFF; gLive.colorAmt = rnd(0.3f, 0.9f);
            gLive.cutoff = rnd(2000, 7000); gLive.reso = rnd(0.1f, 0.4f); gLive.chorus = rnd(0.2f, 0.5f);
            gLive.atk = rnd(0.05f, 0.3f); gLive.dec = 0.3f; gLive.sus = 0.7f; gLive.rel = rnd(0.2f, 0.6f);
            break;
        case 9: // chord pad (one voice = one triad)
            gLive.wave = W_CHORD; gLive.color = C_OFF; gLive.colorAmt = rnd(0.0f, 1.0f);   // major / minor
            gLive.atk = rnd(0.3f, 1.2f); gLive.dec = 0.4f; gLive.sus = 0.8f; gLive.rel = rnd(0.8f, 2.0f);
            gLive.cutoff = rnd(1000, 3000); gLive.reso = rnd(0.1f, 0.3f); gLive.reverbSend = rnd(0.3f, 0.6f); gLive.reverbSize = rnd(0.5f, 0.8f);
            break;
        case 10: // vowel / formant
            gLive.wave = W_SPEECH; gLive.color = C_OFF; gLive.colorAmt = rnd(0.15f, 0.9f);
            gLive.atk = 0.02f; gLive.dec = 0.2f; gLive.sus = 0.7f; gLive.rel = 0.2f;
            gLive.cutoff = 12000; gLive.reso = 0.1f; gLive.reverbSend = rnd(0.1f, 0.3f);
            break;
        case 11: // glass / DX FM
            gLive.wave = W_FM6; gLive.color = C_OFF; gLive.colorAmt = rnd(0.3f, 0.9f);
            gLive.atk = 0.002f; gLive.dec = rnd(0.2f, 0.8f); gLive.sus = 0.0f; gLive.rel = rnd(0.3f, 1.0f);
            gLive.cutoff = 10000; gLive.reso = 0.1f; gLive.reverbSend = rnd(0.3f, 0.6f); gLive.reverbSize = rnd(0.4f, 0.8f);
            break;
        case 12: // grain / texture
            gLive.wave = W_GRAIN; gLive.color = C_OFF; gLive.colorAmt = rnd(0.3f, 0.9f);
            gLive.atk = 0.05f; gLive.dec = 0.3f; gLive.sus = 0.7f; gLive.rel = rnd(0.3f, 0.8f);
            gLive.cutoff = rnd(3000, 9000); gLive.reso = 0.2f; gLive.reverbSend = rnd(0.2f, 0.5f); gLive.reverbSize = rnd(0.4f, 0.7f);
            break;
        case 13: // wave morph (wavetable / additive)
            gLive.wave = (random(2) ? W_WAVETABLE : W_ADDITIVE); gLive.color = C_OFF; gLive.colorAmt = rnd(0.3f, 1.0f);
            gLive.atk = rnd(0.01f, 0.2f); gLive.dec = 0.3f; gLive.sus = 0.7f; gLive.rel = rnd(0.2f, 0.6f);
            gLive.cutoff = rnd(2000, 7000); gLive.reso = rnd(0.1f, 0.4f); gLive.chorus = rnd(0.1f, 0.4f);
            break;
        default: // chip (safety fallback)
            gLive.wave = W_SQUARE; gLive.color = C_OFF; gLive.cutoff = rnd(2000, 8000); gLive.reso = 0.2f;
            gLive.atk = 0.001f; gLive.dec = 0.1f; gLive.sus = 0.5f; gLive.rel = 0.05f; gLive.noise = rnd(0.05f, 0.2f);
            break;
    }
    snprintf(gLive.name, sizeof(gLive.name), "%s-%s-%d", kChars[f], kNouns[random(12)], random(10));
    gDirty = true; pushLive();
}

static void mutate(float amt) {
    checkpoint();
    const float a = 0.05f + 0.45f * amt;
    gLive.cutoff *= rnd(1.f - a, 1.f + a); if (gLive.cutoff < 60) gLive.cutoff = 60; if (gLive.cutoff > 16000) gLive.cutoff = 16000;
    gLive.reso += rnd(-a, a); if (gLive.reso < 0) gLive.reso = 0; if (gLive.reso > 0.95f) gLive.reso = 0.95f;
    gLive.glide = fmaxf(0, gLive.glide + rnd(-a, a));
    gLive.fenvDepth = fmaxf(0, gLive.fenvDepth + rnd(-a, a));
    gLive.delaySend = fmaxf(0, gLive.delaySend + rnd(-a, a));
    gLive.reverbSend = fmaxf(0, gLive.reverbSend + rnd(-a, a));
    gLive.lfo1Rate = fmaxf(0.05f, gLive.lfo1Rate + rnd(-a, a));
    gLive.colorAmt = fmaxf(0, fminf(1, gLive.colorAmt + rnd(-a, a)));   // drives wavetable/grain/swarm/vowel/FM6
    if (random(2)) {
        gLive.wave = (Wave)random(0, 18);
        if ((int)gLive.wave >= W_WAVETABLE) { gLive.color = C_OFF; gLive.colorAmt = rnd(0.2f, 0.9f); }  // colour morph needs colorAmt
        else if (gLive.wave == W_STRING || gLive.wave == W_MODAL) gLive.colorAmt = rnd(0.2f, 0.9f);       // damping / decay
    }
    gDirty = true; pushLive();
}

static void undo() { if (gUndoN > 0) { gLastSound = gLive; gLive = gUndo[--gUndoN]; gDirty = true; pushLive(); } }

// ---- SD save / load -------------------------------------------------------
static const uint32_t kMagic = 0x53545031;  // "STP1"

static void saveToSD() {
    if (!sdBegin()) return;
    char path[32];
    snprintf(path, sizeof(path), "/studio/p%d.gpat", gSlot);
    SD.mkdir("/studio");
    File f = SD.open(path, FILE_WRITE);
    if (!f) return;
    f.write((const uint8_t*)&kMagic, 4);
    f.write((const uint8_t*)&gLive, sizeof(Patch));
    f.close();
}

static void loadFromSD() {
    if (!sdBegin()) return;
    char path[32];
    snprintf(path, sizeof(path), "/studio/p%d.gpat", gSlot);
    File f = SD.open(path, FILE_READ);
    if (!f) return;
    uint32_t m = 0;
    if (f.read((uint8_t*)&m, 4) == 4 && m == kMagic) {
        Patch p;
        if (f.read((uint8_t*)&p, sizeof(Patch)) == sizeof(Patch)) { gLastSound = gLive; gLive = p; gDirty = true; pushLive(); }
    }
    f.close();
}

// ---- drawing --------------------------------------------------------------
static void drawBar(int x, int y, int w, int h, float v, uint16_t c) {
    M5.Display.fillRect(x, y, w, h, themeAt(settingsTheme()).panel);
    int fw = (int)(w * v);
    if (fw > 0) M5.Display.fillRect(x, y, fw, h, c);
}

static void drawScope() {
    // static waveform scope across the top
    const Theme& t = themeAt(settingsTheme());
    const int y0 = 34, h = 34;
    M5.Display.fillRect(0, y0, 240, h, t.bg);
    M5.Display.drawFastHLine(0, y0 + h/2, 240, t.line);
    M5.Display.drawRect(0, y0, 240, h, t.line);
}

static void drawPlay() {
    const Theme& t = themeAt(settingsTheme());
    M5.Display.fillScreen(t.bg);
    // status bar
    M5.Display.setTextColor(t.ink, t.bg);
    M5.Display.setFont(&fonts::Font2);
    char top[64];
    snprintf(top, sizeof(top), "%s", gLive.name);
    M5.Display.drawString(top, 2, 1);
    char keyinfo[24];
    snprintf(keyinfo, sizeof(keyinfo), "%s %s %+d", kNoteNames[gRoot], kScaleNames[gScale], gOctave);
    M5.Display.setTextColor(t.dim, t.bg);
    M5.Display.drawString(keyinfo, 148, 1);
    // badges
    int bx = 2;
    M5.Display.setFont(&fonts::Font0);
    M5.Display.setTextColor(t.ok, t.bg);
    if (gChordMode) { M5.Display.drawString("CHORD", bx, 14); bx += 30; }
    if (gHarmony)   { M5.Display.drawString("HARM", bx, 14); bx += 24; }
    if (gArpMode)   { M5.Display.drawString("ARP", bx, 14); bx += 20; }
    if (gMetro)     { M5.Display.drawString("CLK", bx, 14); bx += 20; }
    if (gTiltOn)    { M5.Display.drawString("TILT", bx, 14); bx += 24; }
    const int ls = looperState();
    if (ls) {
        M5.Display.setTextColor((ls == 2 || ls == 4) ? t.warn : t.ok, t.bg);
        const char* lbl = ls == 1 ? "COUNT" : ls == 2 ? "REC" : ls == 4 ? "OVR" : "LOOP";
        M5.Display.drawString(lbl, bx, 14);
        bx += 28;
    }
    if (gJamMode == 1) {
        M5.Display.setTextColor(t.accent, t.bg);
        char pb[10];
        snprintf(pb, sizeof(pb), gProgN ? "P%d/%d" : "PROG", gProgIdx + 1, gProgN);
        M5.Display.drawString(pb, bx, 14);
        bx += 30;
    }
    if (gDrumMode) {
        M5.Display.setTextColor(t.warn, t.bg);
        M5.Display.drawString("DRUM", bx, 14);
        bx += 24;
    }
    M5.Display.setTextColor(t.warn, t.bg);
    char tmbuf[16];
    snprintf(tmbuf, sizeof(tmbuf), "%d", (int)gTempo);
    M5.Display.drawString(tmbuf, 200, 14);

    drawScope();

    // note grid (4 rows x 10)
    const int gx = 4, gy = 74, cw = 21, ch = 13;
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 10; c++) {
            const int x = gx + c * cw, y = gy + (3 - r) * ch;  // top row = hi
            uint16_t col = t.panel;
            if (gHeld[r][c]) col = t.ok;
            else if (r == 0 && gDrone[c]) col = rgb565(80,130,190);
            M5.Display.fillRect(x, y, cw - 2, ch - 2, col);
        }
    }

    // hint line
    M5.Display.setFont(&fonts::Font0);
    M5.Display.setTextColor(t.dim, t.bg);
    if (gScreen == 1) {
        M5.Display.drawString("q..p sound  k key  s scale  hold k: listen", 2, 128);
    } else {
        M5.Display.drawString("tab menu  alt loop  fn+k hold: listen", 2, 128);
    }
}

static void drawPicker() {
    const Theme& t = themeAt(settingsTheme());
    M5.Display.fillRect(0, 100, 240, 35, t.panel);
    M5.Display.setFont(&fonts::Font2);
    for (int i = 0; i < 10; i++) {
        const int x = 2 + (i % 5) * 48, y = 102 + (i / 5) * 16;
        M5.Display.setTextColor(i == gSlot ? t.accent : t.dim, t.panel);
        char b[16]; snprintf(b, sizeof(b), "%c", "qwertyuiop"[i]);
        M5.Display.drawString(b, x, y);
    }
}

static const char* kMenuItems[6] = {"Randomize", "Mutate", "Undo", "Save SD", "Load SD", "Exit"};
static const int kMenuN = 6;

static void drawMenu() {
    const Theme& t = themeAt(settingsTheme());
    M5.Display.fillScreen(t.bg);
    M5.Display.setFont(&fonts::Font2);
    M5.Display.setTextColor(t.accent, t.bg);
    M5.Display.drawString("CREATE", 2, 2);
    for (int i = 0; i < kMenuN; i++) {
        const int y = 20 + i * 18;
        M5.Display.setTextColor(i == gMenuSel ? t.ink : t.dim, i == gMenuSel ? t.panelHi : t.bg);
        if (i == gMenuSel) M5.Display.fillRect(0, y - 2, 240, 16, t.panelHi);
        M5.Display.drawString(kMenuItems[i], 4, y);
    }
    M5.Display.setFont(&fonts::Font0);
    M5.Display.setTextColor(t.dim, t.bg);
    M5.Display.drawString(";/. move  enter ok  tab back", 2, 128);
}

// ---- control handling -----------------------------------------------------
static void handlePlayKeys() {
    const bool fn = keyHeld(K_FN);

    // entering the fn layer releases any held notes (avoids stuck notes)
    if (keyPressed(K_FN)) {
        Synth::get().allNotesOff();
        for (int r = 0; r < 4; r++) for (int c = 0; c < 10; c++) gHeld[r][c] = false;
    }

    // solo note keys + drone row (skip while fn is held: it is a modifier layer)
    if (!fn) {
        for (int r = 0; r < 4; r++) {
            for (int c = 0; c < 10; c++) {
                const char k = kStr[r][c];
                if (keyPressed(k)) {
                    if (r == 0) { if (gDrumMode) drumTap(c); else if (gJamMode == 1) progAddChord(c); else toggleDrone(c); }
                    else pressKey(r, c);
                } else if (keyReleased(k)) {
                    if (r != 0) releaseKey(r, c);
                }
            }
        }
    }

    // octave / volume / bend / tempo
    if (keyPressed('-')) { gOctave--; if (gOctave < -3) gOctave = -3; }
    if (keyPressed('=')) { gOctave++; if (gOctave > 3) gOctave = 3; }
    if (keyPressed('[')) { gBend -= 1.0f; Synth::get().setBend(gBend); }
    if (keyPressed(']')) { gBend += 1.0f; Synth::get().setBend(gBend); }
    if (keyReleased('[') || keyReleased(']')) { gBend = 0; Synth::get().setBend(0); }
    if (keyPressed('\\')) tapTempo();
    if (keyPressed(K_CTRL)) { gMaster -= 0.07f; if (gMaster < 0) gMaster = 0; pushLive(); }
    if (keyPressed(K_OPT))  { gMaster += 0.07f; if (gMaster > 1) gMaster = 1; pushLive(); }

    // panic / tilt
    if (keyPressed(K_BKSP)) { Synth::get().panic(); for (int c = 0; c < 10; c++) gDrone[c] = false; progClear(); }
    if (keyPressed(K_ENTER)) { gTiltOn = !gTiltOn; if (!gTiltOn) pushLive(); }

    // SD looper (alt tap = record/play/stop; fn+alt = clear)
    if (keyPressed(K_ALT)) { if (keyHeld(K_FN)) looperHold(); else looperTap(); }

    // fn layer
    if (fn) {
        for (int i = 0; i < 10; i++) {
            const char k = "qwertyuiop"[i];
            if (keyPressed(k)) {
                if (keyHeld(K_SHIFT)) { gSlots[i] = gLive; gDirty = false; }
                else { gLastSound = gLive; gSlot = i; gLive = gSlots[i]; gDirty = false; pushLive(); }
            }
        }
        if (keyPressed('k')) { gFnKDown = millis(); gListenDone = false; }
        if (keyHeld('k') && !gListenDone && millis() - gFnKDown > 600) { gListenDone = true; doListen(); }
        if (keyReleased('k') && !gListenDone) gRoot = (gRoot + (keyHeld(K_SHIFT) ? 11 : 1)) % 12;
        if (keyPressed('s')) gScale = (gScale + (keyHeld(K_SHIFT) ? 12 : 1)) % 13;
        if (keyPressed('a')) { gArpMode = (gArpMode + 1) % 4; }
        if (keyPressed('h')) { gHarmony = !gHarmony; }
        if (keyPressed('c')) gChordMode = !gChordMode;
        if (keyPressed('z')) gArpDiv = (gArpDiv + 1) % 4;
        if (keyPressed('x')) gArpSpan2 = !gArpSpan2;
        if (keyPressed('g')) {   // toggle jam mode: drone latch <-> chord progression
            gJamMode = !gJamMode;
            if (gJamMode == 1) {
                for (int c = 0; c < 10; c++) if (gDrone[c]) { gDrone[c] = false; Synth::get().noteOffString(-1, noteAt(0, c)); }
            } else progClear();
        }
        if (keyPressed('d')) { gDrumMode = !gDrumMode; }   // bottom row -> drum pads
        // fn+1..8 -> parameter picker
        for (int i = 0; i < 8; i++) if (keyPressed('1' + i)) gEditParam = (gEditParam == i) ? -1 : i;
    }

    // parameter adjust with [ ]
    if (gEditParam >= 0) {
        const float step = 0.05f;
        if (keyPressed('[') || keyPressed(']')) {
            const float d = keyPressed(']') ? step : -step;
            switch (gEditParam) {
                case 0: gLive.glide = fmaxf(0, gLive.glide + d * 2); break;
                case 1: gLive.atk = fmaxf(0.001f, gLive.atk + d); break;
                case 2: gLive.dec = fmaxf(0.01f, gLive.dec + d); break;
                case 3: gLive.sus = fmaxf(0, fminf(1, gLive.sus + d)); break;
                case 4: gLive.rel = fmaxf(0.02f, gLive.rel + d); break;
                case 5: gLive.wave = (Wave)(((int)gLive.wave + (keyPressed(']') ? 1 : 17)) % 18); break;
                case 6: gLive.cutoff = fmaxf(60, fminf(16000, gLive.cutoff * (keyPressed(']') ? 1.15f : 0.87f))); break;
                case 7: gLive.reso = fmaxf(0, fminf(0.95f, gLive.reso + d)); break;
            }
            gDirty = true; pushLive();
        }
    }

    // tab -> menu
    if (keyPressed(K_TAB)) { gScreen = 2; gMenuSel = 0; }
}

static void handleMenuKeys() {
    if (keyPressed(K_TAB)) { gScreen = 0; return; }
    if (keyPressed(';') || keyPressed('.')) {
        const int d = keyPressed(';') ? -1 : 1;
        gMenuSel = (gMenuSel + d + kMenuN) % kMenuN;
        drawMenu();
    }
    if (keyPressed(K_ENTER)) {
        switch (gMenuSel) {
            case 0: randomize(-1); break;
            case 1: mutate(0.3f); break;
            case 2: undo(); break;
            case 3: saveToSD(); break;
            case 4: loadFromSD(); break;
            case 5: break;
        }
        if (gMenuSel != 5) { /* stay for A/B */ }
        else gScreen = 0;
    }
}

// ---- LISTEN (fn+k hold): mic key/tempo detection --------------------------
static void doListen() {
    Synth& s = Synth::get();
    s.panic();
    const float savedMaster = gMaster;
    s.setMaster(0.f);   // don't listen to ourselves

    const Theme& t = themeAt(settingsTheme());
    M5.Display.fillScreen(t.bg);
    M5.Display.setFont(&fonts::Font2);
    M5.Display.setTextColor(t.accent, t.bg);
    M5.Display.drawString("LISTENING...", 20, 40);
    M5.Display.setFont(&fonts::Font0);
    M5.Display.setTextColor(t.dim, t.bg);
    M5.Display.drawString("hearing the room's key & tempo", 20, 64);

    ListenResult r = listenAnalyse(3000);

    if (r.ok) {
        gRoot = r.root;
        gScale = r.minor ? 10 : 9;      // land on that key's pentatonic
        if (r.bpm > 0) gTempo = r.bpm;
    }
    s.setMaster(savedMaster);
    pushLive();
    drawPlay();
}

// ---- tilt ---------------------------------------------------------------
static float gRoll0 = 0, gPitch0 = 0; static bool gTiltCal = false;
static void tiltTick() {
    if (!gTiltOn || !M5.Imu.isEnabled()) return;
    float ax, ay, az;
    M5.Imu.getAccel(&ax, &ay, &az);
    float roll = atan2f(ay, az) * 57.2958f;
    float pitch = atan2f(-ax, sqrtf(ay*ay + az*az)) * 57.2958f;
    if (!gTiltCal) { gRoll0 = roll; gPitch0 = pitch; gTiltCal = true; }
    const float dr = (roll - gRoll0) / 60.0f;   // -1..1 over 60 deg
    const float dp = (pitch - gPitch0) / 60.0f;
    const float cl = fmaxf(-1, fminf(1, dr));
    switch (gTiltMode) {
        case 1: { Patch p = gLive; p.cutoff = gLive.cutoff * (1.f + 0.8f * cl); pushPatchOnce(p); break; }
        case 2: { Patch p = gLive; p.lfo1Dest = D_PITCH; p.lfo1Rate = 5.f; p.lfo1Amt = 0.4f * fabsf(dp); pushPatchOnce(p); break; }
        case 3: { Synth::get().setMaster(fmaxf(0.05f, gMaster * (1.f + 0.5f * cl))); break; }
        default: { // mode 0: true dual-sound morph (crossfade live <-> last sound)
            const float t = (cl + 1.f) * 0.5f;
            pushPatchOnce(lerpPatch(gLive, gLastSound, t));
            break;
        }
    }
}

static void pushPatchOnce(const Patch& p) { Synth::get().setPatch(p); }
static void metroTick() {
    const uint32_t now = millis();
    const uint32_t beat = (uint32_t)(60000.0f / gTempo);
    if (now - gLastMetro >= beat) {
        gLastMetro = now;
        if (gMetro) Synth::get().click();
        if (gJamMode == 1 && gProgN > 0) {
            if (++gBeatCount >= 4) { gBeatCount = 0; progAdvance(); }
        }
    }
}

static void arpTick() {
    if (!gArpMode || gArpN == 0) return;
    const uint32_t now = millis();
    float div = (gArpDiv == 0) ? 4.f : (gArpDiv == 1) ? 2.f : (gArpDiv == 2) ? 1.333f : 1.f;
    const uint32_t step = (uint32_t)(60000.0f / gTempo / div);
    if (now - gLastArp < step) return;
    gLastArp = now;
    Synth& s = Synth::get();
    if (gArpIdx >= 0) s.noteOffString(-1, gArpNotes[gArpIdx]);   // release the sounding note
    if (gArpMode == 2) gArpIdx = (gArpIdx <= 0) ? (gArpN - 1) : (gArpIdx - 1);  // down (start at top)
    else gArpIdx = (gArpIdx + 1) % gArpN;                                        // up / up-down
    s.noteOnString(-1, gArpNotes[gArpIdx], 0.8f);
}

// ---- app entry ------------------------------------------------------------
void synthAppBegin() {
    audioReassert();   // re-route I2S1 to the codec after the radio used I2S0
    Synth::get().setMute(false);
    looperInit();
    looperSetTempo(gTempo);
    gLive = gSlots[gSlot] = Patch();
    gLastSound = gLive;
    // seed a couple of generative slots
    randomSeed((uint32_t)micros());
    gUndoN = 0;
    pushLive();
    gLastMetro = gLastArp = gLastDraw = gLastTap = millis();
    gScreen = 0;
    M5.Display.setRotation(1);
}

void synthAppStop() {
    Synth::get().panic();
}

void synthAppLoop() {
    const uint32_t now = millis();

    if (gScreen == 0) {
        handlePlayKeys();
        looperSetTempo(gTempo);
        looperTick();
        metroTick();
        arpTick();
        tiltTick();
        if (now - gLastDraw >= 33) { gLastDraw = now; drawPlay(); if (keyHeld(K_FN)) drawPicker(); }
    } else if (gScreen == 2) {
        handleMenuKeys();
        if (now - gLastDraw >= 33) { gLastDraw = now; drawMenu(); }
    }
}
