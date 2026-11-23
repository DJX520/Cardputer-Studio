// keyboard.cpp — self-contained TCA8418 keyboard reader for the Cardputer ADV.
// Talks to the TCA8418 (I2C 0x34) directly over the Wire bus, so it does NOT
// depend on any external TCA8418 library. INT is level-low while the FIFO holds
// events, so we read the pin instead of a GPIO edge, plus a slow I2C fallback.
#include "keyboard.h"
#include "config.h"
#include <M5Unified.h>
#include <Wire.h>

namespace {

constexpr uint32_t kFallbackMs = 100;   // drain over I2C this often even if INT high

// TCA8418 registers (public datasheet addresses)
constexpr uint8_t kRegCfg       = 0x01;
constexpr uint8_t kRegIntStat   = 0x02;
constexpr uint8_t kRegKeyLckEc  = 0x03;
constexpr uint8_t kRegKeyEventA = 0x04;

constexpr uint8_t kCfgKeIen      = 0x01;  // key event interrupt enable
constexpr uint8_t kCfgOvrFlowIen = 0x08;  // overflow interrupt enable
constexpr uint8_t kIntOvrFlow    = 0x08;  // INT_STAT overflow bit

uint64_t gHeld = 0;      // bit (y*14 + x)
uint64_t gPressed = 0;   // edge latch, cleared next poll
uint64_t gReleased = 0;
uint32_t gLastDrain = 0;

// Key code for each physical position (y*14 + x). Reconstructed from the
// Cardputer-Adv 56-key layout (M5Cardputer's own remap).
const uint8_t kKey[4][14] = {
    // row 0
    {'`', '1','2','3','4','5','6','7','8','9','0','-','=', K_BKSP},
    // row 1
    {K_TAB, 'q','w','e','r','t','y','u','i','o','p','[',']', '\\'},
    // row 2
    {K_FN, 'a','s','d','f','g','h','j','k','l',';','\'', K_SHIFT, K_ENTER},
    // row 3
    {K_CTRL, K_OPT, K_ALT, 'z','x','c','v','b','n','m',',','.','/', ' '},
};

int16_t gPos[256];  // code -> bit index, or -1

void tcaWrite(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(kTcaAddr);
    Wire.write(reg);
    Wire.write(val);
    Wire.endTransmission();
}
uint8_t tcaRead(uint8_t reg) {
    Wire.beginTransmission(kTcaAddr);
    Wire.write(reg);
    Wire.endTransmission(false);
    Wire.requestFrom((int)kTcaAddr, 1);
    return Wire.available() ? (uint8_t)Wire.read() : 0;
}

// TCA8418 event (1..80 = row*10 + col + 1) to Cardputer (x,y). Verified on HW.
void toXY(uint8_t code, int8_t& x, int8_t& y) {
    const int raw = (int)(code & 0x7F) - 1, r = raw / 10, c = raw % 10;
    x = (int8_t)(r * 2 + (c > 3 ? 1 : 0));
    y = (int8_t)(c % 4);
}

void apply(uint8_t ev) {
    const uint8_t code = ev & 0x7F;
    if (code < 1 || code > 80) return;
    int8_t x, y;
    toXY(code, x, y);
    if (x < 0 || x >= 14 || y < 0 || y >= 4) return;
    const uint64_t bit = (uint64_t)1 << (y * 14 + x);
    if (ev & 0x80) {           // press
        if (!(gHeld & bit)) { gHeld |= bit; gPressed |= bit; }
    } else {                   // release
        if (gHeld & bit) { gHeld &= ~bit; gReleased |= bit; }
    }
}

}  // namespace

bool keyboardBegin() {
    // presence check: endTransmission returns 0 (ACK) only if the TCA8418 answers
    Wire.beginTransmission(kTcaAddr);
    if (Wire.endTransmission() != 0) return false;
    // enable key events + overflow interrupt (default 8x10 matrix, unused rows/cols idle)
    tcaWrite(kRegCfg, kCfgKeIen | kCfgOvrFlowIen);
    // flush the event FIFO (clear the event counter)
    tcaWrite(kRegKeyLckEc, 0x00);
    pinMode(kKeyIntPin, INPUT);

    for (int i = 0; i < 256; i++) gPos[i] = -1;
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 14; x++) gPos[kKey[y][x]] = (int16_t)(y * 14 + x);
    gHeld = gPressed = gReleased = 0;
    return true;
}

void keyboardPoll() {
    gPressed = 0;
    gReleased = 0;
    const uint32_t now = millis();
    if (digitalRead(kKeyIntPin) == LOW || now - gLastDrain >= kFallbackMs) {
        gLastDrain = now;
        for (int k = 0; k < 16; k++) {   // FIFO holds 10; read a few extra
            const uint8_t ev = tcaRead(kRegKeyEventA);
            if (!ev) break;
            apply(ev);
        }
        const uint8_t st = tcaRead(kRegIntStat);
        if (st & kIntOvrFlow) gHeld = 0;   // dropped events: forget held keys
        if (st) tcaWrite(kRegIntStat, st);
    }
}

bool keyHeld(uint8_t code) {
    int16_t p = gPos[code];
    return p >= 0 && (gHeld & ((uint64_t)1 << p));
}
bool keyPressed(uint8_t code) {
    int16_t p = gPos[code];
    return p >= 0 && (gPressed & ((uint64_t)1 << p));
}
bool keyReleased(uint8_t code) {
    int16_t p = gPos[code];
    return p >= 0 && (gReleased & ((uint64_t)1 << p));
}

uint32_t heldNoteCount() {
    uint32_t n = 0;
    static const char* strings[4] = {
        "1234567890", "qwertyuiop", "asdfghjkl;", "zxcvbnm,./"
    };
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 10; c++) if (keyHeld(strings[r][c])) n++;
    return n;
}

