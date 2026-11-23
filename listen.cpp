// listen.cpp — mic capture + chromagram key detection + tempo estimation.
//
// Hardware path (Cardputer ADV): ES8311 ADC (MEMS mic on MIC1P/N) → ASDOUT
// (GPIO46) → I2S1 RX. The I2S1 port runs full-duplex master: TX drives the DAC,
// RX captures the ADC, sharing BCLK(41)/LRCK(43). The ADC is enabled with the
// M5Unified "Cardputer ADV microphone enable" register sequence.
#include "listen.h"
#include "config.h"
#include "synth_engine.h"
#include <M5Unified.h>
#include <driver/i2s.h>
#include <math.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"

namespace {
constexpr int kSrcRate = 48000;   // codec/I2S rate
constexpr int kRate    = 8000;    // analysis rate (decimate by 6)
constexpr int kDec     = kSrcRate / kRate;   // 6

// ES8311 mic enable / disable (M5Unified Cardputer-Adv microphone sequence).
void micEnable() {
    static const uint8_t on[][2] = {
        {0x01, 0xBF}, {0x0A, 0x0C}, {0x0E, 0x02}, {0x14, 0x10 | 5}, {0x17, 0xBF}, {0x1C, 0x6A}
    };
    for (auto& r : on) M5.In_I2C.writeRegister8(kCodecAddr, r[0], r[1], 400000);
}
void micDisable() {
    static const uint8_t off[][2] = { {0x0E, 0x6A}, {0x01, 0xB5} };
    for (auto& r : off) M5.In_I2C.writeRegister8(kCodecAddr, r[0], r[1], 400000);
}

// Krumhansl-Schmuckler key profiles, indexed C, C#, D ... B.
const float kMajor[12] = {6.35f, 2.23f, 3.48f, 2.33f, 4.38f, 4.09f, 2.52f, 5.19f, 2.39f, 3.66f, 2.29f, 2.88f};
const float kMinor[12] = {6.33f, 2.68f, 3.52f, 5.38f, 2.60f, 3.53f, 2.54f, 4.75f, 3.98f, 2.69f, 3.34f, 3.17f};

// iterative radix-2 FFT (in place)
void fft(float* re, float* im, int n) {
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { float t = re[i]; re[i] = re[j]; re[j] = t; t = im[i]; im[i] = im[j]; im[j] = t; }
    }
    for (int len = 2; len <= n; len <<= 1) {
        const float ang = -6.28318530718f / len;
        const float wRe = cosf(ang), wIm = sinf(ang);
        for (int i = 0; i < n; i += len) {
            float curRe = 1, curIm = 0;
            for (int k = 0; k < len / 2; k++) {
                const float uRe = re[i+k], uIm = im[i+k];
                const float vRe = re[i+k+len/2]*curRe - im[i+k+len/2]*curIm;
                const float vIm = re[i+k+len/2]*curIm + im[i+k+len/2]*curRe;
                re[i+k] = uRe + vRe; im[i+k] = uIm + vIm;
                re[i+k+len/2] = uRe - vRe; im[i+k+len/2] = uIm - vIm;
                const float nRe = curRe*wRe - curIm*wIm;
                curIm = curRe*wIm + curIm*wRe; curRe = nRe;
            }
        }
    }
}
}  // namespace

ListenResult listenAnalyse(int durationMs) {
    ListenResult r;
    const int nSamples = durationMs * kRate / 1000;
    if (nSamples < kRate) return r;

    int16_t* mic = (int16_t*)malloc((size_t)nSamples * 2);
    if (!mic) return r;

    micEnable();
    delay(10);
    // flush stale RX DMA data
    int16_t junk[256]; size_t br;
    for (int i = 0; i < 30; i++) i2s_read((i2s_port_t)kI2sPort, junk, sizeof(junk), &br, pdMS_TO_TICKS(10));

    // capture 48kHz stereo, keep left channel, decimate to kRate
    int got = 0;
    int16_t block[256];   // 128 stereo frames
    while (got < nSamples) {
        if (i2s_read((i2s_port_t)kI2sPort, block, sizeof(block), &br, pdMS_TO_TICKS(100)) != ESP_OK) break;
        const int frames = (int)(br / 4);
        for (int i = 0; i < frames && got < nSamples; i += kDec) mic[got++] = block[i*2];
    }
    micDisable();
    if (got < kRate) { free(mic); return r; }

    // ---- chromagram (1024-point FFT, hop 512) + onset energy ----
    constexpr int FFTSIZE = 1024;
    constexpr int HOP = 512;
    static float w[FFTSIZE]; static bool wInit = false;
    if (!wInit) { for (int i = 0; i < FFTSIZE; i++) w[i] = 0.5f - 0.5f * cosf(6.28318530718f * i / (FFTSIZE - 1)); wInit = true; }
    static float re[FFTSIZE], im[FFTSIZE];

    float chroma[12] = {0};
    const int nFrames = (got - FFTSIZE) / HOP + 1;
    float* energy = (float*)malloc((size_t)(nFrames > 0 ? nFrames : 1) * 4);
    if (!energy) { free(mic); return r; }

    for (int f = 0; f < nFrames; f++) {
        const int16_t* p = mic + f * HOP;
        float e = 0;
        for (int i = 0; i < FFTSIZE; i++) {
            const float x = (float)p[i] * w[i];
            re[i] = x; im[i] = 0;
            e += fabsf(x);
        }
        energy[f] = e;
        fft(re, im, FFTSIZE);
        for (int k = 1; k <= FFTSIZE / 2; k++) {
            const float freq = (float)k * kRate / FFTSIZE;
            if (freq < 50.f || freq > 3500.f) continue;
            const float mag = sqrtf(re[k]*re[k] + im[k]*im[k]);
            const float midi = 69.f + 12.f * logf(freq / 440.f) * 1.442695f;   // *1/ln2
            const int pc = ((int)(midi + 0.5f) % 12 + 12) % 12;
            chroma[pc] += mag;
        }
    }

    // normalise chroma
    float sum = 0; for (int i = 0; i < 12; i++) sum += chroma[i];
    if (sum > 1e-6f) for (int i = 0; i < 12; i++) chroma[i] /= sum;

    // K-S correlation over 24 keys
    float best = -1e9f; int bestRoot = 0; bool bestMinor = true;
    for (int root = 0; root < 12; root++) {
        float mj = 0, mn = 0;
        for (int pc = 0; pc < 12; pc++) {
            const int idx = (pc - root + 12) % 12;
            mj += chroma[pc] * kMajor[idx];
            mn += chroma[pc] * kMinor[idx];
        }
        if (mj > best) { best = mj; bestRoot = root; bestMinor = false; }
        if (mn > best) { best = mn; bestRoot = root; bestMinor = true; }
    }
    r.root = bestRoot; r.minor = bestMinor;

    // ---- tempo from onset-energy autocorrelation ----
    const float frameRate = (float)kRate / HOP;   // ~15.6 Hz
    // subtract mean
    float mean = 0; for (int i = 0; i < nFrames; i++) mean += energy[i]; mean /= (nFrames > 0 ? nFrames : 1);
    for (int i = 0; i < nFrames; i++) energy[i] -= mean;
    const int lagMin = (int)(frameRate * 60.f / 240.f);  // 240 BPM
    const int lagMax = (int)(frameRate * 60.f / 40.f);   // 40 BPM
    float bestCorr = -1e9f; int bestLag = 0;
    for (int lag = lagMin; lag <= lagMax && lag < nFrames; lag++) {
        float corr = 0, nrm = 0;
        for (int i = 0; i + lag < nFrames; i++) { corr += energy[i] * energy[i + lag]; nrm += energy[i]*energy[i] + energy[i+lag]*energy[i+lag]; }
        corr = nrm > 1e-6f ? corr / sqrtf(nrm) : 0;
        if (corr > bestCorr) { bestCorr = corr; bestLag = lag; }
    }
    if (bestCorr > 0.25f && bestLag > 0) r.bpm = 60.f * frameRate / (float)bestLag;

    free(energy);
    free(mic);
    r.ok = true;
    return r;
}
