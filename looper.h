// looper.h — SD long looper: records the synth's output to /studio/loop.pcm
// (48 kHz, 16-bit mono) and loops it back, so loops can be minutes long.
#pragma once

void looperSetTempo(float bpm);
void looperInit();
void looperTap();            // count-in -> record -> play -> overdub -> play -> stop
void looperHold();           // clear the loop
int  looperState();          // 0 idle, 1 count-in, 2 recording, 3 playing, 4 overdub
bool looperHasLoop();

void looperAudioProcess(float* out, int n);   // audio task (core 1)
void looperTick();                            // UI loop (core 0): SD I/O
