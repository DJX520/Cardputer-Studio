// synth_engine.cpp — polyphonic synth, FX, and the ES8311/I2S output path.
// The audio task runs on core 1; the ES8311 codec is configured over I2C on core 0.
#include "synth_engine.h"
#include "looper.h"
#include <M5Unified.h>
#include <driver/i2s.h>
#include <math.h>
#include <xtensa/hal.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// ---- thread safety --------------------------------------------------------
static portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;
#define LOCK()   portENTER_CRITICAL(&g_mux)
#define UNLOCK() portEXIT_CRITICAL(&g_mux)

namespace {
constexpr float kTwoPi = 6.28318530718f;
constexpr float kInv12 = 1.0f / 12.0f;

// ES8311 enable sequence (from M5Unified 0.2.28 _speaker_enabled_cb_cardputer_adv)
const uint8_t kCodecRegs[8][2] = {
    {0x00,0x80},{0x01,0xB5},{0x02,0x18},{0x0D,0x01},
    {0x12,0x00},{0x13,0x10},{0x32,0xBF},{0x37,0x08}
};

// frequency increment table (MIDI 0..127 -> phase step at 48 kHz)
float kInc[129];

// Schroeder reverb (file-static, ~6 KB)
constexpr int kCombLens[4] = {1116, 1188, 1277, 1356};
constexpr int kAllLens[2]  = {556, 441};
float gComb[4][1356];
float gAll[2][556];
float gCombLp[4];
int   gCombW[4] = {0,0,0,0};
int   gAllW[2]  = {0,0};

// morphable wavetable (8 frames x kWaveFrameLen) + granular source buffer
float kWaveTable[8][kWaveFrameLen];
float kGrainBuf[kGrainBufLen];

float polyblep(float t, float dt) {
    if (t < dt) { t /= dt; return t + t - t*t - 1.0f; }
    if (t > 1.0f - dt) { t = (t - 1.0f) / dt; return t*t + t + t + 1.0f; }
    return 0.0f;
}
float fract(float x) { return x - floorf(x); }
float clip1(float x) { return x > 1.f ? 1.f : (x < -1.f ? -1.f : x); }

// lock-free LCG noise for the audio task (never shares Arduino's random()).
uint32_t g_noise = 123456789u;
float fastNoise() {
    g_noise = g_noise * 1664525u + 1013904223u;
    return (float)(g_noise >> 8) / 8388608.0f - 1.0f;  // -1..1
}
}

// ---- audio task + I2S -----------------------------------------------------
static Synth* g_synth = nullptr;
static volatile int g_i2sErr = 0;

static void audioTask(void*) {
    static float mono[kAudioBlock];
    static int16_t buf[kAudioBlock * 2];
    for (;;) {
        const uint32_t t0 = xthal_get_ccount();
        g_synth->processBlock(mono);
        looperAudioProcess(mono, kAudioBlock);
        const uint32_t cyc = xthal_get_ccount() - t0;
        for (int i = 0; i < kAudioBlock; i++) {
            const int16_t s = (int16_t)(clip1(mono[i]) * 32767.0f);
            buf[2*i] = s; buf[2*i+1] = s;
        }
        size_t written = 0;
        const esp_err_t e = i2s_write((i2s_port_t)kI2sPort, buf, sizeof(buf), &written, portMAX_DELAY);
        if (e != ESP_OK && !g_i2sErr) g_i2sErr = e;
        (void)cyc;
    }
}

bool audioBegin(Synth& s) {
    g_synth = &s;
    i2s_config_t cfg = {};
    cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_RX);   // full duplex: DAC + mic ADC
    cfg.sample_rate = kSampleRate;
    cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
    cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
    cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
    cfg.dma_buf_count = 4;
    cfg.dma_buf_len = kAudioBlock;
    cfg.use_apll = false;
    cfg.tx_desc_auto_clear = true;
    if (i2s_driver_install((i2s_port_t)kI2sPort, &cfg, 0, nullptr) != ESP_OK) { g_i2sErr = -1; return false; }
    i2s_pin_config_t pins = {};
    pins.mck_io_num = I2S_PIN_NO_CHANGE;
    pins.bck_io_num = kPinBck;
    pins.ws_io_num  = kPinWs;
    pins.data_out_num = kPinDout;
    pins.data_in_num  = kPinDin;      // 46: codec ASDOUT (mic ADC)
    if (i2s_set_pin((i2s_port_t)kI2sPort, &pins) != ESP_OK) { g_i2sErr = -2; return false; }
    xTaskCreatePinnedToCore(audioTask, "audio", 8192, nullptr, configMAX_PRIORITIES - 2, nullptr, 1);
    return true;
}

void codecBegin() {
    delay(50);  // let BCLK run before configuring the MCLK-less codec
    for (auto& r : kCodecRegs) M5.In_I2C.writeRegister8(kCodecAddr, r[0], r[1], 400000);
}
int audioI2sError() { return g_i2sErr; }

void audioReassert() {
    i2s_pin_config_t pins = {};
    pins.mck_io_num = I2S_PIN_NO_CHANGE;
    pins.bck_io_num = kPinBck;
    pins.ws_io_num  = kPinWs;
    pins.data_out_num = kPinDout;
    pins.data_in_num  = kPinDin;
    i2s_set_pin((i2s_port_t)kI2sPort, &pins);
}

// ---- Synth ----------------------------------------------------------------
Synth& Synth::get() { static Synth s; return s; }

void Synth::begin() {
    for (int i = 0; i <= 128; i++) kInc[i] = 440.0f * powf(2.0f, (i - 69) * kInv12) / kSampleRate;
    for (int i = 0; i < 4; i++) { gCombLp[i] = 0; gCombW[i] = 0; for (int j = 0; j < 1356; j++) gComb[i][j] = 0; }
    for (int i = 0; i < 2; i++) { gAllW[i] = 0; for (int j = 0; j < 556; j++) gAll[i][j] = 0; }
    for (int i = 0; i < kDelaySamples; i++) _delay[i] = 0;
    for (int i = 0; i < kChorusSamples; i++) _chorusL[i] = 0;
    _delayW = 0; _chorusW = 0;
    for (int i = 0; i < kMaxVoices; i++) _v[i].active = false;

    // build the morphable wavetable (8 frames: sine/tri/saw/square/soft/rich/hollow/spaced)
    for (int f = 0; f < 8; f++) {
        for (int i = 0; i < kWaveFrameLen; i++) {
            const float ph = kTwoPi * (float)i / (float)kWaveFrameLen;
            float s = 0;
            for (int h = 1; h <= 8; h++) {
                float w;
                switch (f) {
                    case 0: w = (h == 1) ? 1.0f : 0; break;
                    case 1: w = (h & 1) ? 1.0f / (float)(h*h) : 0; break;
                    case 2: w = 1.0f / h; break;
                    case 3: w = (h & 1) ? 1.0f / h : 0; break;
                    case 4: w = 1.0f / (float)(h*h); break;
                    case 5: w = (h <= 3) ? 1.0f : 0.1f / h; break;
                    case 6: w = (h == 2 || h == 3) ? 1.0f : 0.05f / h; break;
                    default: w = (h % 3 == 1) ? 1.0f / h : 0; break;
                }
                s += w * sinf(ph * h);
            }
            kWaveTable[f][i] = s;
        }
    }
    // granular source: a rising chirp
    float gph = 0;
    for (int i = 0; i < kGrainBufLen; i++) {
        gph += (200.0f + 800.0f * (float)i / (float)kGrainBufLen) / kSampleRate;
        kGrainBuf[i] = sinf(kTwoPi * gph);
    }

    _p = Patch();
}

void Synth::setPatch(const Patch& p) { LOCK(); _p = p; UNLOCK(); }
void Synth::setRoot(int r)      { LOCK(); _root = r & 11; UNLOCK(); }
void Synth::setScale(int s)     { LOCK(); _scale = (s < 0 || s >= 13) ? 0 : s; UNLOCK(); }
void Synth::setOctave(int o)    { LOCK(); _octave = o; UNLOCK(); }
void Synth::setTempo(float b)   { LOCK(); _bpm = b < 40 ? 40 : (b > 300 ? 300 : b); UNLOCK(); }
void Synth::setMaster(float v)  { LOCK(); _master = v; UNLOCK(); }
void Synth::setBend(float s)    { LOCK(); _bend = s; UNLOCK(); }
void Synth::setChromatic(bool c){ LOCK(); _chromatic = c; UNLOCK(); }
void Synth::setMute(bool m)     { LOCK(); _mute = m; UNLOCK(); }
void Synth::click()             { LOCK(); _click = 1.0f; UNLOCK(); }

void Synth::drum(int type, float vel) {
    LOCK();
    const int nv = allocVoice();
    Voice& v = _v[nv];
    v.active = true; v.str = -1; v.drum = (int8_t)type; v.drumAge = 0;
    v.vel = vel; v.ampEnv = 1.0f; v.ampPhase = -1;
    v.p1 = v.p2 = 0; v.drift = 0;
    v.stamp = ++_counter;
    UNLOCK();
}

int Synth::allocVoice() {
    for (int i = 0; i < kMaxVoices; i++) if (!_v[i].active) return i;
    int bi = 0;
    for (int i = 1; i < kMaxVoices; i++) if (_v[i].stamp < _v[bi].stamp) bi = i;
    return bi;
}

void Synth::noteOnString(int str, int midi, float vel) {
    LOCK();
    if (vel > 1.f) vel = 1.f; if (vel < 0.001f) vel = 0.001f;
    _modEnv = 1.0f;   // retrigger the mod envelope
    if (str >= 0 && str < 4) {
        if (_strN[str] < 4) _strStack[str][_strN[str]++] = midi;
        const int top = _strStack[str][_strN[str] - 1];
        const int vi = _voiceForStr[str];
        if (vi >= 0 && _v[vi].active) {
            _v[vi].midi = top; _v[vi].vel = vel; _v[vi].gate = true;
            if (_v[vi].ampPhase < 0) { _v[vi].ampPhase = 0; _v[vi].ampEnv = 0; }
        } else {
            const int nv = allocVoice();
            Voice& v = _v[nv];
            v.active = true; v.str = str; v.midi = top; v.curMidi = top;
            v.vel = vel; v.gate = true; v.ampEnv = 0; v.ampPhase = 0;
            v.fenv = 0; v.fenvPhase = 0; v.ic1 = v.ic2 = 0;
            v.p1 = v.p1b = v.p1c = v.p2 = v.fmPh = 0;
            v.drum = -1; v.drumAge = 0;
            v.pluck = true;
            v.driftPhase = (float)random(6283) / 1000.0f;
            v.stamp = ++_counter;
            _voiceForStr[str] = nv;
        }
    } else {
        const int nv = allocVoice();
        Voice& v = _v[nv];
        v.active = true; v.str = -1; v.midi = midi; v.curMidi = midi;
        v.vel = vel; v.gate = true; v.ampEnv = 0; v.ampPhase = 0;
        v.fenv = 0; v.fenvPhase = 0; v.ic1 = v.ic2 = 0;
        v.p1 = v.p1b = v.p1c = v.p2 = v.fmPh = 0;
        v.drum = -1; v.drumAge = 0;
        v.pluck = true;
        v.driftPhase = (float)random(6283) / 1000.0f;
        v.stamp = ++_counter;
    }
    UNLOCK();
}

void Synth::noteOffString(int str, int midi) {
    LOCK();
    if (str >= 0 && str < 4) {
        int n = _strN[str];
        for (int i = 0; i < n; i++) if (_strStack[str][i] == midi) {
            for (int j = i; j < n - 1; j++) _strStack[str][j] = _strStack[str][j+1];
            _strN[str]--; break;
        }
        const int vi = _voiceForStr[str];
        if (vi < 0 || !_v[vi].active) { UNLOCK(); return; }
        if (_strN[str] > 0) {
            _v[vi].midi = _strStack[str][_strN[str] - 1];  // pull-off: glide back
        } else {
            _v[vi].gate = false;
            if (_v[vi].ampPhase < 3) _v[vi].ampPhase = 3;
            _voiceForStr[str] = -1;
        }
    } else {
        for (int i = 0; i < kMaxVoices; i++) {
            Voice& v = _v[i];
            if (v.active && v.str < 0 && v.midi == midi && v.gate) {
                v.gate = false; if (v.ampPhase < 3) v.ampPhase = 3;
            }
        }
    }
    UNLOCK();
}

void Synth::allNotesOff() {
    LOCK();
    for (int i = 0; i < kMaxVoices; i++) { _v[i].gate = false; if (_v[i].ampPhase < 3) _v[i].ampPhase = 3; }
    for (int s = 0; s < 4; s++) { _strN[s] = 0; _voiceForStr[s] = -1; }
    UNLOCK();
}

void Synth::panic() {
    LOCK();
    for (int i = 0; i < kMaxVoices; i++) { _v[i].active = false; _v[i].gate = false; }
    for (int s = 0; s < 4; s++) { _strN[s] = 0; _voiceForStr[s] = -1; }
    UNLOCK();
}

static float lfoValue(LfoShape sh, float ph) {
    ph = fract(ph);
    switch (sh) {
        case L_TRI:    return ph < 0.5f ? 4*ph - 1 : 3 - 4*ph;
        case L_SAW:    return 2*ph - 1;
        case L_SQUARE: return ph < 0.5f ? 1.f : -1.f;
        case L_SH:     return sinf(kTwoPi * ph);  // S&H approximated as sine (fine for v1)
        default:       return sinf(kTwoPi * ph);
    }
}

// One sample of one voice; advances per-voice state.
float Synth::renderVoiceSample(int v, const Patch& p, float l1, float l2) {
    Voice& V = _v[v];
    const float invSr = 1.0f / kSampleRate;

    // one-shot drums (skip the normal voice path)
    if (V.drum >= 0) {
        V.drumAge += invSr;
        switch (V.drum) {
            case 0: { // kick: sine pitch sweep 160->40 Hz
                const float f = 40.f + 120.f * expf(-V.drumAge * 45.f);
                V.p1 += f / kSampleRate; V.p1 = fract(V.p1);
                V.ampEnv *= 0.999f;
                if (V.ampEnv < 0.002f) V.active = false;
                return sinf(kTwoPi * V.p1) * V.ampEnv * V.vel * 1.3f;
            }
            case 1: { // snare: tone + noise
                V.p2 += 190.f / kSampleRate; V.p2 = fract(V.p2);
                V.ampEnv *= 0.998f;
                if (V.ampEnv < 0.002f) V.active = false;
                return (sinf(kTwoPi * V.p2) * 0.4f + fastNoise() * 0.8f) * V.ampEnv * V.vel;
            }
            case 2: { // hihat: differentiated noise (high-pass)
                const float x = fastNoise();
                const float hp = x - V.drift; V.drift = x;
                V.ampEnv *= 0.985f;
                if (V.ampEnv < 0.002f) V.active = false;
                return hp * V.ampEnv * V.vel * 1.6f;
            }
            default: { // clap: three noise bursts
                const float t = V.drumAge;
                float env = 0;
                for (int k = 0; k < 3; k++) { const float d = t - k * 0.013f; env += expf(-d * d * 9000.f); }
                if (t > 0.06f) V.active = false;
                return fastNoise() * env * V.vel * 0.9f;
            }
        }
    }

    // physical modeling sources: Karplus-Strong string / modal resonator
    if (p.wave == W_STRING || p.wave == W_MODAL) {
        if (V.pluck) {
            V.pluck = false;
            if (p.wave == W_STRING) { for (int i = 0; i < kKarplusLen; i++) V.ksDelay[i] = fastNoise(); V.ksW = 0; V.ksLast = 0; }
            else { for (int m = 0; m < 4; m++) { V.mdY1[m] = 1.0f; V.mdY2[m] = 0; } }
        }
        const float g = p.glide > 0.0005f ? 1.0f / (p.glide * kSampleRate) : 1e9f;
        const float d = (float)V.midi - V.curMidi;
        if (fabsf(d) <= g) V.curMidi = V.midi; else V.curMidi += (d > 0 ? g : -g);
        const int mI = (int)V.curMidi; const float fr = V.curMidi - mI;
        const int mA = mI < 0 ? 0 : (mI > 127 ? 127 : mI);
        const int mB = (mI + 1) > 127 ? 127 : (mI + 1);
        const float freq = (kInc[mA] + (kInc[mB] - kInc[mA]) * fr) * kSampleRate;
        float s;
        if (p.wave == W_STRING) {
            const float period = fminf(kSampleRate / freq, (float)(kKarplusLen - 1));
            const float damping = 0.955f + 0.04f * p.colorAmt;   // colorAmt up = brighter/longer
            const int ip = (int)period; const float fp = period - ip;
            const int r0 = (V.ksW - ip + kKarplusLen) % kKarplusLen;
            const int r1 = (r0 - 1 + kKarplusLen) % kKarplusLen;
            const float out = V.ksDelay[r0] * (1 - fp) + V.ksDelay[r1] * fp;
            V.ksDelay[V.ksW] = (out + V.ksLast) * 0.5f * damping;
            V.ksW = (V.ksW + 1) % kKarplusLen;
            V.ksLast = out;
            s = out;
        } else {
            static const float kRatio[4] = {1.0f, 2.0f, 3.0f, 4.2f};
            const float rr = 0.9975f + 0.0022f * p.colorAmt;
            const float r2 = rr * rr;
            s = 0;
            for (int m = 0; m < 4; m++) {
                const float f = freq * kRatio[m];
                if (f > kSampleRate * 0.45f) break;
                const float c = 2.0f * rr * cosf(kTwoPi * f / kSampleRate);
                const float y = c * V.mdY1[m] - r2 * V.mdY2[m];
                V.mdY2[m] = V.mdY1[m]; V.mdY1[m] = y;
                s += y * (1.0f / (float)(m + 1));
            }
        }
        V.drumAge += invSr;
        if (!V.gate && V.drumAge > 2.0f) V.active = false;
        return s * V.vel * 0.6f;
    }

    // pitch + glide
    const float glideRate = p.glide > 0.0005f ? 1.0f / (p.glide * kSampleRate) : 1e9f;
    const float d = (float)V.midi - V.curMidi;
    if (fabsf(d) <= glideRate) V.curMidi = V.midi;
    else V.curMidi += (d > 0 ? glideRate : -glideRate);
    const int mi = (int)V.curMidi;
    const float frac = V.curMidi - mi;
    const int m1 = mi < 0 ? 0 : (mi > 127 ? 127 : mi);
    const int m2 = (mi + 1) > 127 ? 127 : (mi + 1);
    float inc = kInc[m1] + (kInc[m2] - kInc[m1]) * frac;

    // pitch modulation (bend + vibrato + drift)
    float pitchMod = _bend;
    if (p.lfo1Dest == D_PITCH) pitchMod += l1 * p.lfo1Amt * 2.0f;
    if (p.lfo2Dest == D_PITCH) pitchMod += l2 * p.lfo2Amt * 2.0f;
    V.driftPhase += invSr * 0.9f; if (V.driftPhase > kTwoPi) V.driftPhase -= kTwoPi;
    V.drift += (sinf(V.driftPhase) - V.drift) * 0.002f;
    pitchMod += V.drift * p.drift * 0.12f;
    if (_chromatic) pitchMod += 0; // chromatic handled by note mapping
    inc *= (1.0f + 0.057762f * pitchMod);
    const float dt = inc;

    // amp ADSR
    float amp = 0;
    if (V.ampPhase >= 0) {
        switch ((int)V.ampPhase) {
            case 0: V.ampEnv += invSr / (p.atk < 0.001f ? 0.001f : p.atk);
                    if (V.ampEnv >= 1.f) { V.ampEnv = 1.f; V.ampPhase = 1; } amp = V.ampEnv; break;
            case 1: V.ampEnv -= invSr / (p.dec < 0.001f ? 0.001f : p.dec);
                    if (V.ampEnv <= p.sus) { V.ampEnv = p.sus; V.ampPhase = 2; } amp = V.ampEnv; break;
            case 2: amp = p.sus; break;
            default: V.ampEnv -= invSr / (p.rel < 0.001f ? 0.001f : p.rel);
                     if (V.ampEnv <= 0) { V.ampEnv = 0; V.active = false; } amp = V.ampEnv; break;
        }
    }
    // filter envelope (AD)
    float fenv = 0;
    if (V.fenvPhase >= 0) {
        if ((int)V.fenvPhase == 0) {
            V.fenv += invSr / (p.fenvAtk < 0.001f ? 0.001f : p.fenvAtk);
            if (V.fenv >= 1.f) { V.fenv = 1.f; V.fenvPhase = 1; } fenv = V.fenv;
        } else {
            V.fenv -= invSr / (p.fenvDec < 0.001f ? 0.001f : p.fenvDec);
            if (V.fenv < 0) V.fenv = 0; fenv = V.fenv;
        }
    }

    // oscillator + colour
    float wphase = V.p1;
    switch (p.color) {
        case C_FM: case C_FM2: case C_FM_BELL: {
            const float ratio = (p.color == C_FM) ? 2.0f : (p.color == C_FM2 ? 3.0f : 7.0f);
            V.fmPh += inc * ratio; V.fmPh = fract(V.fmPh);
            const float depth = p.colorAmt * 6.0f * (1.0f - 0.5f * fenv);
            wphase = fract(V.p1 + sinf(kTwoPi * V.fmPh) * depth * 0.16f);
            break;
        }
        case C_SYNC: {
            // hard sync reuses p1c; skip it for waves that already use p1c as a
            // detuned/interval phase (FAT / CHORD / SWARM / FM6 / STRING_MACHINE)
            const bool p1cBusy = (p.wave == W_FAT || p.wave == W_CHORD || p.wave == W_SWARM ||
                                  p.wave == W_FM6 || p.wave == W_STRING_MACHINE);
            if (!p1cBusy) {
                V.p1c += inc * 1.5f;
                if (V.p1c >= 1.f) { V.p1c = fract(V.p1c); V.p1 = 0; wphase = 0; }
            }
            break;
        }
        default: break;
    }
    float s;
    switch (p.wave) {
        case W_SINE:   s = sinf(kTwoPi * wphase); break;
        case W_TRI:    s = wphase < 0.5f ? wphase*4 - 1 : 3 - 4*wphase; break;
        case W_SAW:    s = 2*wphase - 1 - polyblep(wphase, dt); break;
        case W_SQUARE: { float q = wphase < 0.5f ? 1 : -1; q += polyblep(wphase, dt) - polyblep(fract(wphase + 0.5f), dt); s = q * 0.6f; } break;
        case W_PWM: {
            const float pw = 0.3f + 0.2f * l1;
            float q = wphase < pw ? 1 : -1;
            q += polyblep(wphase, dt) - polyblep(fract(wphase + (1-pw)), dt);
            s = q * 0.6f; break;
        }
        case W_WAVETABLE: {   // morphable wavetable (colorAmt = frame position)
            const float pos = p.colorAmt * 7.0f;
            const int f0 = (int)pos; const int f1 = f0 < 7 ? f0 + 1 : 7;
            const float fp = pos - f0;
            const float rp = wphase * (float)kWaveFrameLen;
            const int i0 = (int)rp; const int i1 = (i0 + 1) & (kWaveFrameLen - 1); const float fi = rp - i0;
            const float a = kWaveTable[f0][i0] * (1 - fp) + kWaveTable[f1][i0] * fp;
            const float b = kWaveTable[f0][i1] * (1 - fp) + kWaveTable[f1][i1] * fp;
            s = a + (b - a) * fi; break;
        }
        case W_GRAIN: {       // granular cloud (3 grains over a chirp buffer)
            if (V.pluck) {
                V.pluck = false;
                for (int g = 0; g < 3; g++) { V.grainEnv[g] = 0; V.grainRate[g] = invSr / 0.05f; V.grainPh[g] = (fastNoise() + 1.0f) * 0.5f; }
            }
            s = 0;
            for (int g = 0; g < 3; g++) {
                V.grainPh[g] += inc * (1.0f + 0.06f * (float)(g - 1));
                V.grainPh[g] = fract(V.grainPh[g]);
                const float rp = V.grainPh[g] * (float)kGrainBufLen;
                const int i0 = (int)rp; const int i1 = (i0 + 1) & (kGrainBufLen - 1); const float fi = rp - i0;
                const float gs = kGrainBuf[i0] * (1 - fi) + kGrainBuf[i1] * fi;
                V.grainEnv[g] += V.grainRate[g];
                if (V.grainEnv[g] >= 1.0f) {   // respawn the grain
                    V.grainEnv[g] = 0;
                    V.grainRate[g] = invSr / (0.02f + 0.15f * p.colorAmt);
                    V.grainPh[g] = (fastNoise() + 1.0f) * 0.5f;
                }
                s += gs * (0.5f - 0.5f * cosf(kTwoPi * V.grainEnv[g]));  // Hann window
            }
            s *= 0.5f; break;
        }
        case W_ADDITIVE: {   // additive: 6 partials (CPU-balanced), rolloff morphs via colorAmt
            const float roll = 1.0f + (1.0f - p.colorAmt) * 3.0f;
            s = 0;
            for (int h = 1; h <= 6; h++) s += sinf(kTwoPi * wphase * (float)h) / (1.0f + (float)(h * h) * (roll - 1.0f));
            s *= 0.3f; break;
        }
        case W_CHORD: {      // three-note chord in one voice (root + 3rd + 5th)
            const float third = (p.colorAmt < 0.5f) ? 1.26f : 1.189f;   // major / minor 3rd
            V.p1b += inc * third; V.p1b = fract(V.p1b);
            V.p1c += inc * 1.498f; V.p1c = fract(V.p1c);                // perfect 5th
            const float a = 2*wphase - 1 - polyblep(wphase, dt);
            const float b = 2*V.p1b - 1 - polyblep(V.p1b, inc*third);
            const float c = 2*V.p1c - 1 - polyblep(V.p1c, inc*1.498f);
            s = (a + b + c) * 0.33f; break;
        }
        case W_SWARM: {      // swarm: 5 detuned saws (supersaw cloud)
            const float dd = 0.004f + 0.02f * p.colorAmt;
            const float d0 = inc * (1.f - 2*dd), d1 = inc * (1.f - dd), d2 = inc * (1.f + dd), d3 = inc * (1.f + 2*dd);
            V.p1b += d1; V.p1b = fract(V.p1b);
            V.p1c += d2; V.p1c = fract(V.p1c);
            V.grainPh[0] += d0; V.grainPh[0] = fract(V.grainPh[0]);
            V.grainPh[1] += d3; V.grainPh[1] = fract(V.grainPh[1]);
            const float sa = 2*wphase - 1 - polyblep(wphase, dt);
            const float sb = 2*V.p1b - 1 - polyblep(V.p1b, d1);
            const float sc = 2*V.p1c - 1 - polyblep(V.p1c, d2);
            const float sd0 = 2*V.grainPh[0] - 1 - polyblep(V.grainPh[0], d0);
            const float sd1 = 2*V.grainPh[1] - 1 - polyblep(V.grainPh[1], d3);
            s = (sa + sb + sc + sd0 + sd1) * 0.2f; break;
        }
        case W_SPEECH: {     // formant synthesis: 3 resonators (vowel morph via colorAmt)
            const float v = p.colorAmt;
            const float fs[3] = {400.f + 400.f * v, 800.f + 1200.f * v, 2400.f + 800.f * v};
            const float exc = (2*wphase - 1) * 0.5f + fastNoise() * 0.08f;
            const float rr = 0.99f, r2 = rr * rr;
            s = 0;
            for (int m = 0; m < 3; m++) {
                const float c = 2.0f * rr * cosf(kTwoPi * fs[m] / kSampleRate);
                const float y = c * V.mdY1[m] - r2 * V.mdY2[m] + exc;
                V.mdY2[m] = V.mdY1[m]; V.mdY1[m] = y;
                s += y;
            }
            s *= 0.33f; break;
        }
        case W_FM6: {        // 3-op DX-style FM (carrier + 2 modulators)
            const float depth = p.colorAmt * 8.0f;
            V.p1b += inc * 2.0f; V.p1b = fract(V.p1b);
            V.p1c += inc * 3.0f; V.p1c = fract(V.p1c);
            const float mod1 = sinf(kTwoPi * V.p1b);
            const float mod2 = sinf(kTwoPi * V.p1c);
            s = sinf(kTwoPi * (wphase + (mod1 + mod2 * 0.5f) * depth * 0.15f));
            break;
        }
        case W_WAVE_TERRAIN: {   // 2D wave terrain: sin(x)*sin(y), orbit morphs via colorAmt
            const float x = sinf(kTwoPi * wphase);
            V.p1b += inc * (1.0f + 0.3f * p.colorAmt); V.p1b = fract(V.p1b);
            const float y = sinf(kTwoPi * V.p1b);
            s = x * y * 1.5f; break;
        }
        case W_STRING_MACHINE: { // divide-down string machine: root + 5th + octave (detuned)
            V.p1b += inc * 1.498f * 1.001f; V.p1b = fract(V.p1b);
            V.p1c += inc * 2.0f * 0.999f;  V.p1c = fract(V.p1c);
            const float a = 2*wphase - 1 - polyblep(wphase, dt);
            const float b = 2*V.p1b - 1 - polyblep(V.p1b, inc*1.498f*1.001f);
            const float c = 2*V.p1c - 1 - polyblep(V.p1c, inc*2.0f*0.999f);
            s = (a * 0.5f + b * 0.3f + c * 0.2f) * 0.7f; break;
        }
        case W_CHIPTUNE: {       // NES pulse (duty via colorAmt) + triangle
            const float duty = 0.125f + 0.5f * p.colorAmt;
            float q = wphase < duty ? 1 : -1;
            q += polyblep(wphase, dt) - polyblep(fract(wphase + (1-duty)), dt);
            V.grainPh[2] += inc; V.grainPh[2] = fract(V.grainPh[2]);
            const float tri = (V.grainPh[2] < 0.5f) ? V.grainPh[2]*4 - 1 : 3 - 4*V.grainPh[2];
            s = q * 0.5f + tri * 0.4f; break;
        }
        default: { // W_FAT: three detuned saws
            const float dd = p.fatDet * 0.5f / 1200.0f;
            V.p1b += inc * (1.f - dd); V.p1b = fract(V.p1b);
            V.p1c += inc * (1.f + dd); V.p1c = fract(V.p1c);
            const float a = 2*wphase - 1 - polyblep(wphase, dt);
            const float b = 2*V.p1b - 1 - polyblep(V.p1b, inc*(1.f-dd));
            const float c = 2*V.p1c - 1 - polyblep(V.p1c, inc*(1.f+dd));
            s = (a + b + c) * 0.4f; break;
        }
    }
    // post colour
    switch (p.color) {
        case C_FOLD:  s = tanhf(s * (1.f + 4.f * p.colorAmt)); break;
        case C_RING:  V.fmPh += inc * 2.f; V.fmPh = fract(V.fmPh); s *= sinf(kTwoPi * V.fmPh); break;
        case C_PHASE: { const float q = wphase * 2 - 1; s = q / (1.f + fabsf(q) * p.colorAmt * 4.f) * 2.f; } break;
        default: break;
    }
    // sub oscillator
    if (p.sub > 0.001f) {
        V.p2 += inc * 0.5f; V.p2 = fract(V.p2);
        float sq = V.p2 < 0.5f ? 1 : -1;
        sq += polyblep(V.p2, inc*0.5f) - polyblep(fract(V.p2 + 0.5f), inc*0.5f);
        s += sq * 0.5f * p.sub;
    }
    // noise
    if (p.noise > 0.001f) {
        s += fastNoise() * p.noise * (0.3f + 0.7f * fenv);
    }
    // drive
    s = tanhf(s * (0.5f + p.drive * 3.5f));

    // filter (SVF)
    float cutoff = p.cutoff + (p.fenvDepth > 0 ? p.fenvDepth * fenv * 12000.0f : 0);
    if (p.lfo1Dest == D_CUTOFF) cutoff *= (1.f + l1 * p.lfo1Amt * 0.8f);
    if (p.lfo2Dest == D_CUTOFF) cutoff *= (1.f + l2 * p.lfo2Amt * 0.8f);
    if (p.modAmt > 0.001f) cutoff *= (1.f + _modEnv * p.modAmt);
    if (cutoff < 60.f) cutoff = 60.f; if (cutoff > 16000.f) cutoff = 16000.f;
    {
        const float g = tanf(3.14159265f * cutoff / kSampleRate);
        const float k = 2.0f - 1.8f * (p.reso > 0.95f ? 0.95f : p.reso);
        const float a1 = 1.0f / (1.0f + g*(g + k));
        const float a2 = g * a1, a3 = g * a2;
        const float v3 = s - V.ic2;
        const float v1 = a1*V.ic1 + a2*v3;
        const float v2 = V.ic2 + a2*V.ic1 + a3*v3;
        V.ic1 = 2*v1 - V.ic1;
        V.ic2 = 2*v2 - V.ic2;
        switch (p.fmode) {
            case F_HP:    s = s - k*v1 - v2; break;
            case F_BP:    s = v1; break;
            case F_NOTCH: s = s - k*v1; break;
            default:      s = v2; break;
        }
    }

    // amp + tremolo + vel
    float ampOut = amp * V.vel;
    if (p.lfo1Dest == D_AMP) ampOut *= 1.f - 0.4f * p.lfo1Amt * (0.5f - 0.5f*l1);
    if (p.lfo2Dest == D_AMP) ampOut *= 1.f - 0.4f * p.lfo2Amt * (0.5f - 0.5f*l2);

    // advance phase
    V.p1 = fract(V.p1 + inc);

    return s * ampOut;
}

void Synth::applyFx(float* out, int n, float master, bool mute) {
    const float invSr = 1.0f / kSampleRate;
    const float damp = 0.2f + 0.3f * _p.reverbSize;
    const float fb = 0.55f + 0.35f * _p.reverbSize;
    _delayLen = (int)(_p.delayTime * kSampleRate);
    if (_delayLen < 1) _delayLen = 1;
    if (_delayLen >= kDelaySamples) _delayLen = kDelaySamples - 1;

    for (int i = 0; i < n; i++) {
        const float dry = out[i];

        // chorus (modulated short delay, linear interp)
        _chorusLfo += invSr * 0.7f; if (_chorusLfo > 1.f) _chorusLfo -= 1.f;
        const float tap = 6.0f + 6.0f * sinf(kTwoPi * _chorusLfo);
        int ci = _chorusW - (int)tap; if (ci < 0) ci += kChorusSamples;
        int ci1 = ci - 1; if (ci1 < 0) ci1 += kChorusSamples;
        const float cfrac = tap - floorf(tap);
        const float chorus = _chorusL[ci] * (1-cfrac) + _chorusL[ci1] * cfrac;
        _chorusL[_chorusW] = dry;
        _chorusW = (_chorusW + 1) % kChorusSamples;

        // delay
        int di = _delayW - _delayLen; if (di < 0) di += kDelaySamples;
        const float dout = _delay[di];
        _delay[_delayW] = dry + dout * _p.delayFb;
        _delayW = (_delayW + 1) % kDelaySamples;

        // reverb (Schroeder: 4 combs -> 2 allpass)
        float rv = 0;
        for (int c = 0; c < 4; c++) {
            const float outC = gComb[c][gCombW[c]];
            const float lp = outC * (1.f - damp) + gCombLp[c] * damp;
            gCombLp[c] = lp;
            gComb[c][gCombW[c]] = dry + lp * fb;
            gCombW[c] = (gCombW[c] + 1) % kCombLens[c];
            rv += outC;
        }
        rv *= 0.25f;
        for (int a = 0; a < 2; a++) {
            const float bout = gAll[a][gAllW[a]];
            const float y = -rv + bout;
            gAll[a][gAllW[a]] = rv + bout * 0.5f;
            gAllW[a] = (gAllW[a] + 1) % kAllLens[a];
            rv = y;
        }

        float outS = dry
            + chorus * _p.chorus
            + dout * _p.delaySend
            + rv * _p.reverbSend * 0.7f;

        out[i] = mute ? 0.f : clip1(outS * master);
        // metronome click (short noise blip, decays quickly)
        if (_click > 0.001f) {
            out[i] += fastNoise() * _click * 0.35f;
            _click *= 0.996f;
        } else _click = 0;
    }
}

void Synth::processBlock(float* out) {
    LOCK();
    const Patch p = _p;
    const float bpm = _bpm, master = _master;
    const bool mute = _mute;
    UNLOCK();

    const float invSr = 1.0f / kSampleRate;
    // mod envelope decay
    const float modDecay = expf(-invSr / (p.modDec < 0.001f ? 0.001f : p.modDec));
    _modEnv *= modDecay;

    for (int i = 0; i < kAudioBlock; i++) {
        // global LFOs
        _lfo1t += p.lfo1Rate * invSr; if (_lfo1t > 1.f) _lfo1t -= 1.f;
        _lfo2t += p.lfo2Rate * invSr; if (_lfo2t > 1.f) _lfo2t -= 1.f;
        const float l1 = lfoValue(p.lfo1Shape, _lfo1t);
        const float l2 = lfoValue(p.lfo2Shape, _lfo2t);

        float acc = 0;
        for (int v = 0; v < kMaxVoices; v++) if (_v[v].active) acc += renderVoiceSample(v, p, l1, l2);
        out[i] = acc * 0.25f;
    }

    applyFx(out, kAudioBlock, master, mute);
    (void)bpm;
}
