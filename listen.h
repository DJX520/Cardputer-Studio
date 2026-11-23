// listen.h — "LISTEN": room key/mode/tempo detection through the ES8311 mic.
// Captures mic audio, builds a chromagram, finds the tonic + major/minor with
// the Krumhansl-Schmuckler profiles, and estimates tempo from the onset energy.
#pragma once

struct ListenResult {
    bool  ok = false;
    int   root = 9;      // 0..11 (C..B)
    bool  minor = true;  // true = minor, false = major
    float bpm = 0;       // 0 when no confident beat
};

// Capture ~durationMs of room audio and analyse (blocking). Returns ok=false on
// mic/heap failure. The synth must be muted by the caller while this runs.
ListenResult listenAnalyse(int durationMs);
