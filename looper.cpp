// looper.cpp — SD long looper with overdub.
//
// Layers on SD: /studio/loop.pcm (base) and /studio/loop.1 (overdub, merged on
// close). Two single-producer/single-consumer rings bridge the audio task
// (core 1) and the SD I/O (core 0). Overdub records the live performance (not
// the loop) while the base plays, then mixes it into the base on close.
#include "looper.h"
#include "config.h"
#include "sd_util.h"
#include <SD.h>
#include <math.h>

#define RING 4800                     // int16 samples (~100 ms at 48 kHz)

static int16_t gRec[RING], gPlay[RING];
static volatile int gRecW = 0, gRecR = 0;
static volatile int gPlayW = 0, gPlayR = 0;

static File gFileBase, gFileDub;
static bool  gBaseOpen = false, gDubOpen = false, gDubExists = false;
static uint32_t gLoopBytes = 0;        // base loop bytes (== overdub length)
static uint32_t gDubBytes = 0;         // overdub bytes recorded so far
static int  gState = 0;                // 0 idle, 1 count-in, 2 rec, 3 play, 4 overdub
static uint32_t gCountStart = 0, gLastBeat = 0;
static float gBpm = 100;
static bool  gHasLoop = false;

static int recAvail()  { return (gRecW - gRecR + RING) % RING; }
static int recSpace()  { return RING - 1 - recAvail(); }
static int playAvail() { return (gPlayW - gPlayR + RING) % RING; }
static int playSpace() { return RING - 1 - playAvail(); }

static int16_t clampi(float x) { if (x > 32767.f) return 32767; if (x < -32768.f) return -32768; return (int16_t)x; }

void looperSetTempo(float bpm) { gBpm = bpm; }
void looperInit() {
    gState = 0; gRecW = gRecR = gPlayW = gPlayR = 0; gLoopBytes = 0; gHasLoop = false; gDubExists = false;
    if (sdBegin()) {
        File f = SD.open("/studio/loop.pcm", FILE_READ);
        if (f) { gHasLoop = true; gLoopBytes = f.size(); f.close(); }
        if (gHasLoop) { File d = SD.open("/studio/loop.1", FILE_READ); if (d) { gDubExists = true; d.close(); } }
    }
}

int looperState() { return gState; }
bool looperHasLoop() { return gHasLoop; }

static void closeFiles() {
    if (gBaseOpen) { gFileBase.close(); gBaseOpen = false; }
    if (gDubOpen)  { gFileDub.close();  gDubOpen = false; }
}

static void openBaseRead() { gFileBase = SD.open("/studio/loop.pcm", FILE_READ); gBaseOpen = (bool)gFileBase; }

// mix the overdub layer into the base, then remove it
static void mergeDub() {
    File b = SD.open("/studio/loop.pcm", FILE_READ);
    File d = SD.open("/studio/loop.1", FILE_READ);
    if (!b || !d) { if (b) b.close(); if (d) d.close(); return; }
    File t = SD.open("/studio/loop.tmp", FILE_WRITE);
    if (!t) { b.close(); d.close(); return; }
    int16_t sb[256], sd[256];
    for (;;) {
        const size_t rb = b.read((uint8_t*)sb, sizeof(sb));
        const size_t rd = d.read((uint8_t*)sd, sizeof(sd));
        const int n = (int)(rb / 2);
        for (int i = 0; i < n; i++) sb[i] = clampi((float)sb[i] + ((i * 2 < (int)rd) ? (float)sd[i] : 0.f));
        t.write((uint8_t*)sb, (size_t)n * 2);
        if (rb < sizeof(sb)) break;
    }
    b.close(); d.close(); t.close();
    SD.remove("/studio/loop.pcm");
    SD.rename("/studio/loop.tmp", "/studio/loop.pcm");
    SD.remove("/studio/loop.1");
    gDubExists = false;
}

// ---- UI side (core 0) -----------------------------------------------------
void looperTap() {
    switch (gState) {
        case 0:
            if (gHasLoop) {                       // replay saved loop
                sdBegin(); openBaseRead();
                if (gDubExists) { gFileDub = SD.open("/studio/loop.1", FILE_READ); gDubOpen = (bool)gFileDub; }
                gState = 3;
            } else {                              // record new base
                gState = 1; gCountStart = millis(); gLastBeat = 0;
            }
            break;
        case 1: gState = 0; break;                // cancel count-in
        case 2: {                                 // stop base recording -> play
            while (recAvail() > 0) {
                int n = recAvail(); if (n > 240) n = 240;
                gLoopBytes += gFileBase.write((uint8_t*)&gRec[gRecR], n * 2);
                gRecR = (gRecR + n) % RING;
            }
            closeFiles();
            gHasLoop = true;
            openBaseRead();
            gState = 3;
            break;
        }
        case 3: {                                 // start overdub (from loop top)
            closeFiles();
            gFileDub = SD.open("/studio/loop.1", FILE_WRITE); gDubOpen = (bool)gFileDub;
            gDubBytes = 0;
            openBaseRead();                       // replay base from 0 while overdubbing
            gState = 4;
            break;
        }
        case 4:                                   // overdub done early: merge + play
            closeFiles();
            mergeDub();
            openBaseRead();
            if (gDubExists) { gFileDub = SD.open("/studio/loop.1", FILE_READ); gDubOpen = (bool)gFileDub; }
            gState = 3;
            break;
    }
}

void looperHold() {
    closeFiles();
    if (sdBegin()) { SD.remove("/studio/loop.pcm"); SD.remove("/studio/loop.1"); }
    gHasLoop = false; gDubExists = false; gLoopBytes = 0; gState = 0;
    gRecW = gRecR = gPlayW = gPlayR = 0;
}

void looperTick() {
    const uint32_t now = millis();
    if (gState == 1) {                              // count-in
        const uint32_t beat = (uint32_t)(60000.0f / gBpm);
        if (now - gLastBeat >= beat) {
            gLastBeat = now;
            if (now - gCountStart >= beat * 4) {
                if (sdBegin()) {
                    SD.mkdir("/studio");
                    gFileBase = SD.open("/studio/loop.pcm", FILE_WRITE);
                    gBaseOpen = (bool)gFileBase;
                    gLoopBytes = 0;
                    gState = gBaseOpen ? 2 : 0;
                } else gState = 0;
            }
        }
    } else if (gState == 2 && gBaseOpen) {          // record base
        while (recAvail() > 0) {
            const int n = recAvail() > 240 ? 240 : recAvail();
            const size_t w = gFileBase.write((const uint8_t*)&gRec[gRecR], n * 2);
            gLoopBytes += w;
            gRecR = (gRecR + (int)(w / 2)) % RING;
            if (w == 0) break;
        }
    } else if (gState == 3) {                       // play: base (+ dub) summed
        while (playSpace() > 240 && gFileBase.available()) {
            int16_t tmp[240];
            const size_t rb = gFileBase.read((uint8_t*)tmp, sizeof(tmp));
            const int n = (int)(rb / 2);
            if (n == 0) break;
            int16_t* dst = &gPlay[gPlayW];
            for (int i = 0; i < n; i++) dst[i] = tmp[i];
            if (gDubOpen && gDubExists) {
                const size_t rd = gFileDub.read((uint8_t*)tmp, (size_t)n * 2);
                const int nd = (int)(rd / 2);
                for (int i = 0; i < nd; i++) dst[i] = clampi((float)dst[i] + (float)tmp[i]);
            }
            gPlayW = (gPlayW + n) % RING;
        }
        if (!gFileBase.available() || (int)gFileBase.position() >= (int)gLoopBytes) {
            gFileBase.seek(0);
            if (gDubOpen && gDubExists) gFileDub.seek(0);
        }
    } else if (gState == 4) {                       // overdub: play base, record dub
        while (playSpace() > 240 && gFileBase.available()) {
            int16_t tmp[240];
            const size_t rb = gFileBase.read((uint8_t*)tmp, sizeof(tmp));
            const int n = (int)(rb / 2);
            if (n == 0) break;
            int16_t* dst = &gPlay[gPlayW];
            for (int i = 0; i < n; i++) dst[i] = tmp[i];
            gPlayW = (gPlayW + n) % RING;
        }
        if (!gFileBase.available() || (int)gFileBase.position() >= (int)gLoopBytes) gFileBase.seek(0);
        while (gDubOpen && recAvail() > 0) {
            const int n = recAvail() > 240 ? 240 : recAvail();
            const size_t w = gFileDub.write((const uint8_t*)&gRec[gRecR], n * 2);
            gDubBytes += w;
            gRecR = (gRecR + (int)(w / 2)) % RING;
            if (w == 0) break;
        }
        if (gDubBytes >= gLoopBytes) {              // reached the loop length: merge + play
            closeFiles();
            mergeDub();
            gDubExists = false;
            openBaseRead();
            gState = 3;
        }
    }
}

// ---- audio side (core 1) --------------------------------------------------
void looperAudioProcess(float* out, int n) {
    const bool rec  = (gState == 2 || gState == 4);
    const bool play = (gState == 3 || gState == 4);
    if (rec) {                                       // record the live sound (no loop)
        for (int i = 0; i < n; i++) {
            if (recSpace() <= 0) break;
            float v = out[i]; if (v > 1.f) v = 1.f; if (v < -1.f) v = -1.f;
            gRec[gRecW] = (int16_t)(v * 32767.0f);
            gRecW = (gRecW + 1) % RING;
        }
    }
    if (play) {                                      // mix the loop under the live sound
        for (int i = 0; i < n; i++) {
            if (playAvail() <= 0) break;
            out[i] += (float)gPlay[gPlayR] / 32768.0f;
            gPlayR = (gPlayR + 1) % RING;
        }
    }
}
