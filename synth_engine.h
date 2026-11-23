// synth_engine.h — real-time polyphonic synth engine + I2S/ES8311 output.
// One audio task on core 1 renders the voices; the UI (core 0) drives notes and
// parameters through a small spinlock. No PSRAM: all buffers are static.
#pragma once
#include <cstdint>
#include "config.h"

// ---- Patch (one named sound) ----------------------------------------------
enum Wave : uint8_t { W_SINE, W_TRI, W_SAW, W_SQUARE, W_PWM, W_FAT, W_STRING, W_MODAL, W_WAVETABLE, W_GRAIN, W_ADDITIVE, W_CHORD, W_SWARM, W_SPEECH, W_FM6, W_WAVE_TERRAIN, W_STRING_MACHINE, W_CHIPTUNE };
enum Color : uint8_t { C_OFF, C_FM, C_FM2, C_FM_BELL, C_PHASE, C_FOLD, C_SYNC, C_RING };
enum FMode : uint8_t { F_LP, F_HP, F_BP, F_NOTCH };
enum LfoShape : uint8_t { L_SINE, L_TRI, L_SAW, L_SQUARE, L_SH };
enum LfoDest : uint8_t { D_PITCH, D_CUTOFF, D_AMP, D_FENV };

struct Patch {
    Wave   wave    = W_SAW;
    Color  color   = C_OFF;
    float  colorAmt= 0.0f;
    FMode  fmode   = F_LP;
    float  cutoff  = 4000.0f;   // Hz
    float  reso    = 0.30f;
    float  fatDet  = 0.12f;     // fat detune (normalised 0..1)
    float  glide   = 0.120f;    // seconds
    float  atk=0.005f, dec=0.120f, sus=0.70f, rel=0.250f;
    float  fenvAtk=0.005f, fenvDec=0.180f, fenvDepth=0.0f;
    float  sub=0.0f, noise=0.0f, drive=1.0f, vibrato=0.0f;
    // FX
    float  chorus=0.0f, delaySend=0.0f, delayTime=0.30f, delayFb=0.35f, reverbSend=0.0f, reverbSize=0.5f;
    // LFO 1 / 2 (rate in Hz; the UI converts tempo divisions to Hz)
    LfoShape lfo1Shape=L_SINE; float lfo1Rate=5.0f; LfoDest lfo1Dest=D_PITCH; float lfo1Amt=0.0f;
    LfoShape lfo2Shape=L_TRI;  float lfo2Rate=0.5f; LfoDest lfo2Dest=D_CUTOFF; float lfo2Amt=0.0f;
    float  modAtk=0.010f, modDec=0.300f, modAmt=0.0f;   // mod envelope -> cutoff
    float  drift=0.04f;        // per-voice pitch drift (normalised)
    char   name[24] = "INIT";
};

// ---- Engine ---------------------------------------------------------------
class Synth {
public:
    static Synth& get();

    void begin();                          // reset voices + FX
    void processBlock(float* out);         // audio task only

    // Note API (any task; thread-safe).
    void noteOnString(int str, int midi, float vel);   // str 0..3 (glide) or -1 free
    void noteOffString(int str, int midi);
    void allNotesOff();
    void panic();

    // Parameters (thread-safe).
    void setPatch(const Patch& p);
    void setRoot(int root);
    void setScale(int scale);
    void setOctave(int oct);
    void setTempo(float bpm);
    void setMaster(float v);              // 0..1
    void setBend(float semis);
    void setChromatic(bool on);
    void setMute(bool on);
    void click();                          // one-shot metronome click (noise blip)
    void drum(int type, float vel);        // one-shot drum: 0 kick, 1 snare, 2 hat, 3 clap

    float cpuLoad() const { return _cpu; }

private:
    int   allocVoice();
    float renderVoiceSample(int v, const Patch& p, float l1, float l2);
    void  applyFx(float* out, int n, float master, bool mute);

    struct Voice {
        bool  active=false, gate=false;
        int   str=-1;             // string index or -1 free
        int   midi=60;            // target MIDI note
        float curMidi=60.0f;      // gliding current pitch
        float vel=0;
        float ampEnv=0, ampPhase=-1;   // ADSR (-1 idle, 0 A, 1 D, 2 S, 3 R)
        float fenv=0, fenvPhase=-1;    // filter envelope (-1 idle, 0 A, 1 D)
        float ic1=0, ic2=0;            // SVF state
        float p1=0, p1b=0, p1c=0;      // osc / detuned / sync-master phases
        float p2=0;                    // sub phase
        float fmPh=0;                  // FM / ring modulator phase
        float drift=0, driftPhase=0;
        int8_t drum=-1;                // -1 normal voice, else drum type (0..3)
        float drumAge=0;
        bool pluck=false;              // seed the physical model on note-on
        float ksDelay[kKarplusLen]; int ksW=0; float ksLast=0;   // Karplus-Strong string
        float mdY1[4]={0,0,0,0}, mdY2[4]={0,0,0,0};       // modal resonator (4 modes)
        float grainPh[3]={0,0,0}, grainEnv[3]={1,1,1}, grainRate[3]={0,0,0};   // granular (3 grains)
        uint32_t stamp=0;
    };

    Voice _v[kMaxVoices];
    uint32_t _counter=0;
    int   _voiceForStr[4] = {-1,-1,-1,-1};
    int   _strStack[4][4]; int _strN[4] = {0,0,0,0};
    Patch _p;
    int   _root=9, _scale=10, _octave=0;   // A minor pentatonic
    float _bpm=100, _master=0.7f, _bend=0;
    float _lfo1t=0, _lfo2t=0, _modEnv=0;
    bool  _chromatic=false, _mute=false;
    float _cpu=0;
    float _click=0;

    // FX state (reverb buffers are file-static in synth_engine.cpp)
    float _delay[kDelaySamples];
    int   _delayW=0, _delayLen=kDelaySamples;
    float _chorusL[kChorusSamples];
    int   _chorusW=0;
    float _chorusLfo=0;
};

// ---- audio output (called from main.cpp) ----------------------------------
bool audioBegin(Synth& s);
void codecBegin();
int  audioI2sError();
void audioReassert();   // re-route I2S1 to the codec pins after the radio used I2S0
