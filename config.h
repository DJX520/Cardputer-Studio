// ============================================================================
//  Cardputer Studio — unified music firmware for the M5Stack Cardputer ADV
//  config.h : hardware facts, constants, feature toggles
//
//  Hardware reference: M5Stack Cardputer-Adv (K132-Adv)
//    - ESP32-S3FN8, 240 MHz dual core, 8 MB flash, NO PSRAM, ~512 KB SRAM
//    - ES8311 mono codec on I2S (BCLK G41 / LRCK G43 / DIN G42 / DOUT G46)
//    - ST7789V2 240x135 IPS display
//    - TCA8418 keyboard (I2C 0x34, INT G11)
//    - BMI270 IMU (I2C 0x69)
// ============================================================================
#pragma once
#include <cstdint>

// ---- Audio ---------------------------------------------------------------
constexpr int   kSampleRate   = 48000;   // Hz
constexpr int   kAudioBlock   = 120;     // samples per audio block == one DMA buffer
constexpr int   kI2sPort      = 1;       // synth uses I2S1; radio uses I2S0 (Audio lib)
constexpr int   kPinBck       = 41;
constexpr int   kPinWs        = 43;
constexpr int   kPinDout      = 42;
constexpr int   kPinDin       = 46;      // codec -> S3 (microphone), reserved
constexpr uint8_t kCodecAddr  = 0x18;    // ES8311 I2C address

// ---- Keyboard (TCA8418) --------------------------------------------------
constexpr uint8_t kTcaAddr    = 0x34;
constexpr int   kKeyIntPin    = 11;
constexpr int   kCols         = 14;
constexpr int   kRows         = 4;

// ---- Display -------------------------------------------------------------
constexpr int   kScreenW      = 240;
constexpr int   kScreenH      = 135;

// ---- Synth engine --------------------------------------------------------
constexpr int   kMaxVoices    = 8;
constexpr int   kMaxSlots     = 10;
constexpr int   kMaxLoopEvents= 2048;
constexpr int   kMaxChordSteps= 8;       // chord progression length

// ---- FX buffer lengths (samples @ 48 kHz) --------------------------------
constexpr int   kDelaySamples = 12000;   // 0.25 s tempo-synced / free delay
constexpr int   kReverbSamples= 6144;    // Schroeder room (4 comb + 2 allpass)
constexpr int   kChorusSamples= 1152;    // modulated chorus tap (24 ms)

// ---- physical-model / synthesis buffer lengths ---------------------------
constexpr int   kKarplusLen   = 512;     // Karplus-Strong delay line (samples)
constexpr int   kWaveFrameLen = 256;     // wavetable frame length (samples)
constexpr int   kGrainBufLen  = 1024;    // granular source buffer (samples)

// ---- Tilt (IMU) ----------------------------------------------------------
constexpr float kTiltReachDeg = 60.f;    // degrees of lean for a full morph

// ---- Misc ----------------------------------------------------------------
constexpr int   kMaxStations  = 20;      // web radio stations
constexpr int   kMaxNameLen   = 30;
constexpr int   kMaxUrlLen    = 120;
