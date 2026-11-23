// sd_util.h — Cardputer ADV microSD mount (SPI SCLK=40 MISO=39 MOSI=14, CS=12).
// G5 is the Cap LoRa-1262 NSS on the same bus; hold it high so the radio stays
// deselected (harmless when no cap is fitted).
#pragma once
#include <SD.h>
#include <SPI.h>

inline bool sdBegin() {
    static bool started = false;
    if (!started) {
        pinMode(5, OUTPUT);
        digitalWrite(5, HIGH);
        SPI.begin(40, 39, 14, 12);
        started = true;
    }
    return SD.begin(12, SPI, 25000000, "/sd", 4);
}
