// keyboard.h — robust TCA8418 keyboard reader for the Cardputer ADV (56 keys).
// Polls the INT level and drains the FIFO ourselves, so multiple simultaneous
// keys (chords) work and the device never locks up (the M5Cardputer reader can
// drop an event between its read and clear on the ADV).
#pragma once
#include <cstdint>

// Special key codes (printable ASCII keys are reported as their char).
enum : uint8_t {
    K_FN    = 0x80,
    K_SHIFT = 0x81,
    K_CTRL  = 0x82,
    K_OPT   = 0x83,
    K_ALT   = 0x84,
    K_TAB   = 0x85,
    K_BKSP  = 0x86,
    K_ENTER = 0x87,
};

bool keyboardBegin();                       // call after M5.begin()
void keyboardPoll();                        // drain FIFO + update held/pressed sets
bool keyHeld(uint8_t code);                 // ASCII char or K_* code
bool keyPressed(uint8_t code);              // edge: newly pressed since last poll
bool keyReleased(uint8_t code);             // edge: newly released since last poll
uint32_t heldNoteCount();                   // number of note keys currently held
