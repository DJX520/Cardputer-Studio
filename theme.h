// theme.h — unified UI theme: RGB565 palettes + tiny drawing helpers.
// Header-only (inline). All colours are 16-bit RGB565.
#pragma once
#include <cstdint>
#include "config.h"

inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

struct Theme {
    uint16_t bg;      // screen background
    uint16_t panel;   // raised surface / cards
    uint16_t panelHi; // selected surface
    uint16_t ink;     // primary text
    uint16_t dim;     // secondary text
    uint16_t line;    // borders / grid
    uint16_t accent;  // highlight / active
    uint16_t ok;      // success / playing
    uint16_t warn;    // caution / recording
    uint16_t bad;     // error / clipping
};

// Six coherent palettes. Palette 0 = default ("Studio", a dark warm theme).
inline const Theme& themeAt(int i) {
    static const Theme kThemes[6] = {
        { // 0 Studio (dark warm)
            rgb565(18,16,14),  rgb565(30,27,22),  rgb565(48,43,34),
            rgb565(232,222,200), rgb565(138,130,112), rgb565(64,58,46),
            rgb565(255,166,58), rgb565(106,214,120), rgb565(255,196,74),
            rgb565(255,84,64) },
        { // 1 Paper (grey LCD, Teenage Engineering style)
            rgb565(196,198,184), rgb565(182,184,170), rgb565(150,153,139),
            rgb565(29,31,26), rgb565(94,97,87), rgb565(143,146,134),
            rgb565(255,77,26), rgb565(70,150,70), rgb565(200,150,20),
            rgb565(200,40,30) },
        { // 2 Op-1 dark
            rgb565(11,11,12),  rgb565(16,17,19),  rgb565(42,44,48),
            rgb565(236,233,226), rgb565(123,127,133), rgb565(58,60,66),
            rgb565(255,106,43), rgb565(96,200,120), rgb565(255,200,80),
            rgb565(255,70,60) },
        { // 3 Ocean
            rgb565(8,14,24),   rgb565(14,22,36),  rgb565(24,36,54),
            rgb565(210,224,238), rgb565(110,130,150), rgb565(40,56,80),
            rgb565(64,190,255), rgb565(90,220,150), rgb565(255,200,90),
            rgb565(255,90,90) },
        { // 4 Forest
            rgb565(10,18,12),  rgb565(16,26,17),  rgb565(26,40,26),
            rgb565(214,232,210), rgb565(120,145,115), rgb565(40,58,38),
            rgb565(120,220,90), rgb565(150,230,120), rgb565(230,200,80),
            rgb565(235,80,70) },
        { // 5 Mono
            rgb565(0,0,0),     rgb565(16,16,16),  rgb565(40,40,40),
            rgb565(255,255,255), rgb565(140,140,140), rgb565(70,70,70),
            rgb565(255,255,255), rgb565(180,180,180), rgb565(220,220,220),
            rgb565(255,120,120) },
    };
    i = (i < 0 || i >= 6) ? 0 : i;
    return kThemes[i];
}
inline const char* themeName(int i) {
    static const char* k[6] = {"Studio","Paper","OP-1","Ocean","Forest","Mono"};
    return k[(i < 0 || i >= 6) ? 0 : i];
}
