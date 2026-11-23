// ============================================================================
//  Cardputer Studio — a unified music firmware for the M5Stack Cardputer ADV
//
//  Three integrated apps behind one launcher and one UI theme:
//    SYNTH     — playable polyphonic synth (scale-locked strings, glide,
//                chord mode, harmony, arp, drones, metronome, looper, tilt,
//                ten sound slots, randomize/mutate/undo, SD save/load)
//    RADIO     — internet radio (WiFi stream + SD station list + level meter)
//    SETTINGS  — theme, brightness, WiFi manager, reset
//
//  Merges and reworks, with the owners' designs credited:
//    - CardputerModularSynth (MIT)          — audio path, keyboard reader, scales
//    - GLIDE-Synth-Cardputer-ADV (PolyForm) — playable synth feature set
//    - WebRadio_WuSiU_Cardputer_Adv (MIT)   — streaming radio
//  This project's own code is MIT; see README for full credits and licences.
// ============================================================================
#include <M5Unified.h>
#include "config.h"
#include "theme.h"
#include "keyboard.h"
#include "synth_engine.h"
#include "synth_app.h"
#include "radio.h"
#include "settings.h"

enum Mode : uint8_t { M_LAUNCHER = 0, M_SYNTH = 1, M_RADIO = 2, M_SETTINGS = 3 };
static Mode gMode = M_LAUNCHER;
static int  gLauncherSel = 0;

static const char* kModeNames[3] = {"SYNTH", "RADIO", "SETTINGS"};
static const char* kModeDesc[3]  = {
    "playable synth + jam",
    "internet radio",
    "theme / wifi / reset"
};

static void drawLauncher() {
    const Theme& t = themeAt(settingsTheme());
    M5.Display.fillScreen(t.bg);
    M5.Display.setFont(&fonts::Font2);
    M5.Display.setTextColor(t.accent, t.bg);
    M5.Display.drawString("CARD STUDIO", 2, 4);
    M5.Display.setFont(&fonts::Font0);
    M5.Display.setTextColor(t.dim, t.bg);
    M5.Display.drawString("M5Stack Cardputer ADV", 2, 18);

    for (int i = 0; i < 3; i++) {
        const int y = 40 + i * 30;
        const bool sel = i == gLauncherSel;
        if (sel) M5.Display.fillRoundRect(4, y - 2, 232, 26, 4, t.panelHi);
        M5.Display.setFont(&fonts::Font2);
        M5.Display.setTextColor(sel ? t.accent : t.ink, sel ? t.panelHi : t.bg);
        M5.Display.drawString(kModeNames[i], 12, y);
        M5.Display.setFont(&fonts::Font0);
        M5.Display.setTextColor(sel ? t.dim : t.dim, sel ? t.panelHi : t.bg);
        M5.Display.drawString(kModeDesc[i], 96, y + 6);
    }

    M5.Display.setFont(&fonts::Font0);
    M5.Display.setTextColor(t.dim, t.bg);
    M5.Display.drawString(";/. move  enter open", 2, 128);
}

static void enterMode(Mode m) {
    switch (m) {
        case M_SYNTH:    synthAppBegin();    break;
        case M_RADIO:    radioAppBegin();    break;
        case M_SETTINGS: settingsAppBegin(); break;
        default: break;
    }
    gMode = m;
}

static void leaveMode(Mode m) {
    switch (m) {
        case M_SYNTH:    synthAppStop();    break;
        case M_RADIO:    radioAppStop();    break;
        case M_SETTINGS: settingsAppStop(); break;
        default: break;
    }
}

void setup() {
    auto cfg = M5.config();
    cfg.internal_spk = false;   // we own the ES8311 codec
    cfg.internal_mic = false;
    M5.begin(cfg);
    M5.Display.setRotation(1);

    if (!keyboardBegin()) {
        M5.Display.setFont(&fonts::Font2);
        M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
        M5.Display.drawString("keyboard init failed", 10, 10);
    }
    settingsLoad();

    Synth::get().begin();
    if (!audioBegin(Synth::get())) {
        M5.Display.setFont(&fonts::Font2);
        M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
        M5.Display.drawString("audio init failed", 10, 30);
    }
    codecBegin();

    drawLauncher();
}

void loop() {
    keyboardPoll();

    switch (gMode) {
        case M_LAUNCHER:
            if (keyPressed(';')) gLauncherSel = (gLauncherSel + 2) % 3;
            if (keyPressed('.')) gLauncherSel = (gLauncherSel + 1) % 3;
            if (keyPressed(K_ENTER)) { enterMode((Mode)(M_SYNTH + gLauncherSel)); }
            break;
        case M_SYNTH:
            synthAppLoop();
            if (keyPressed('`')) { leaveMode(M_SYNTH); gMode = M_LAUNCHER; drawLauncher(); }
            break;
        case M_RADIO:
            radioAppLoop();
            if (keyPressed('`')) { leaveMode(M_RADIO); gMode = M_LAUNCHER; drawLauncher(); }
            break;
        case M_SETTINGS:
            settingsAppLoop();
            if (keyPressed('`') || keyPressed(K_TAB)) { leaveMode(M_SETTINGS); gMode = M_LAUNCHER; drawLauncher(); }
            break;
    }
    delay(1);
}
