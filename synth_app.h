// synth_app.h — performance synth app (note mapping, chords, arp, looper, tilt,
// slots, UI). Drives Synth (the audio engine) from the keyboard + IMU.
#pragma once

void synthAppBegin();   // enter synth mode (configure Synth from settings)
void synthAppLoop();    // one frame; called from the main loop
void synthAppStop();    // leave synth mode (release all notes)
