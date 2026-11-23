// radio.cpp — internet radio, ported from the WebRadio project and restyled to
// the unified theme. Streams MP3/AAC via the ESP32-audioI2S library (I2S0 on the
// ES8311 pins); the station list comes from /station_list.txt on SD or the
// built-in list. A real level meter replaces the old fake FFT.
#include "radio.h"
#include "config.h"
#include "theme.h"
#include "keyboard.h"
#include "settings.h"
#include "synth_engine.h"
#include "sd_util.h"
#include <M5Unified.h>
#include <WiFi.h>
#include "Audio.h"

#define MAX_ST 20
#define MAX_NAME 30
#define MAX_URL 120

struct Station { char name[MAX_NAME]; char url[MAX_URL]; };
static Station gSt[MAX_ST];
static int gStN = 0, gCur = 0;
static int gVol = 12;              // Audio lib volume 0..21
static bool gMute = false;
static bool gList = false; static int gListSel = 0;
static uint32_t gLastDraw = 0, gLastMeter = 0;

static Audio audio;

// built-in default stations (used when no SD /station_list.txt)
static const Station kDefaults[] = {
    {"SomaFM Groove Salad", "https://ice1.somafm.com/groovesalad-128-mp3"},
    {"SomaFM Drone Zone",   "https://ice1.somafm.com/dronezone-128-mp3"},
};

// drive the (optional LoRa cap) NSS high so the SD shares the SPI bus cleanly
static void loadStations() {
    gStN = 0;
    if (sdBegin()) {
        File f = SD.open("/station_list.txt");
        if (f) {
            String line;
            while (f.available() && gStN < MAX_ST) {
                line = f.readStringUntil('\n');
                int c = line.indexOf(',');
                if (c > 0) {
                    String n = line.substring(0, c); n.trim();
                    String u = line.substring(c + 1); u.trim();
                    if (n.length() && u.length()) {
                        strncpy(gSt[gStN].name, n.c_str(), MAX_NAME - 1);
                        strncpy(gSt[gStN].url, u.c_str(), MAX_URL - 1);
                        gSt[gStN].name[MAX_NAME-1] = 0; gSt[gStN].url[MAX_URL-1] = 0;
                        gStN++;
                    }
                }
            }
            f.close();
        }
    }
    if (gStN == 0) {
        const int n = sizeof(kDefaults)/sizeof(kDefaults[0]);
        for (int i = 0; i < n; i++) { gSt[gStN++] = kDefaults[i]; }
    }
}

static void playStation() {
    audio.stopSong();
    audio.connecttohost(gSt[gCur].url);
}

static void setVol() {
    audio.setVolume(gMute ? 0 : gVol);
}

void radioAppBegin() {
    Synth::get().setMute(true);   // silence the synth voice
    M5.Display.setRotation(1);
    wifiEnsureConnected();
    audio.setPinout(kPinBck, kPinWs, kPinDout);
    audio.setVolume(gVol);
    loadStations();
    gList = false;
    playStation();
}

void radioAppStop() {
    audio.stopSong();
}

static void drawRadio() {
    const Theme& t = themeAt(settingsTheme());
    M5.Display.fillScreen(t.bg);
    M5.Display.setFont(&fonts::Font2);
    M5.Display.setTextColor(t.accent, t.bg);
    M5.Display.drawString("RADIO", 2, 1);
    M5.Display.setTextColor(t.ink, t.bg);
    char b[40]; snprintf(b, sizeof(b), "%d/%d", gCur + 1, gStN);
    M5.Display.drawString(b, 200, 1);

    M5.Display.setTextColor(t.ink, t.bg);
    M5.Display.drawString(gSt[gCur].name, 2, 20);
    M5.Display.setTextColor(t.dim, t.bg);
    String u = gSt[gCur].url;
    if (u.length() > 40) u = u.substring(0, 40);
    M5.Display.setFont(&fonts::Font0);
    M5.Display.drawString(u, 2, 40);

    // volume bar
    M5.Display.drawRect(2, 62, 236, 8, t.line);
    M5.Display.fillRect(3, 63, (int)(234 * gVol / 21.0f), 6, gMute ? t.bad : t.ok);
    M5.Display.setFont(&fonts::Font0);
    M5.Display.setTextColor(t.dim, t.bg);
    M5.Display.drawString(gMute ? "MUTE" : "VOL", 2, 74);

    if (gList) {
        M5.Display.fillRect(0, 84, 240, 50, t.panel);
        for (int i = 0; i < 3; i++) {
            const int idx = gListSel - 1 + i;
            if (idx < 0 || idx >= gStN) continue;
            const int y = 86 + i * 16;
            const bool sel = idx == gListSel;
            if (sel) M5.Display.fillRect(0, y - 1, 240, 15, t.panelHi);
            M5.Display.setTextColor(sel ? t.ink : t.dim, sel ? t.panelHi : t.panel);
            M5.Display.drawString(gSt[idx].name, 2, y);
        }
        M5.Display.setTextColor(t.dim, t.panel);
        M5.Display.drawString(";/. move  enter select  l close", 2, 130);
    } else {
        M5.Display.setTextColor(t.dim, t.bg);
        M5.Display.drawString(",/. station  /; vol  m mute  l list  tab back", 2, 130);
    }
}

static void drawMeter() {
    const Theme& t = themeAt(settingsTheme());
    // activity meter from the input-buffer fill (fluctuates while streaming)
    const uint32_t fill = audio.inBufferFilled();   // 0..~60000 bytes
    const int level = (int)(fill * 100 / 40000);
    const int h = level > 100 ? 30 : (level < 0 ? 0 : level * 30 / 100);
    M5.Display.fillRect(0, 94, 240, 34, t.bg);
    for (int x = 0; x < 240; x += 6) {
        const int bh = (x / 6) % 8 < 4 ? (3 + (x/6)%5) : (2 + (x/6)%4);
        M5.Display.fillRect(x, 128 - bh, 5, bh, t.line);
    }
    M5.Display.fillRect(0, 128 - h, 240, h, t.accent);
    M5.Display.fillRect(0, 128 - h, 240, 2, t.ink);
}

void radioAppLoop() {
    audio.loop();
    const uint32_t now = millis();

    if (gList) {
        if (keyPressed(';')) { gListSel = (gListSel - 1 + gStN) % gStN; }
        if (keyPressed('.')) { gListSel = (gListSel + 1) % gStN; }
        if (keyPressed(K_ENTER)) { gCur = gListSel; playStation(); gList = false; }
        if (keyPressed('l')) gList = false;
    } else {
        if (keyPressed('/')) { gCur = (gCur + 1) % gStN; playStation(); }
        if (keyPressed(',')) { gCur = (gCur - 1 + gStN) % gStN; playStation(); }
        if (keyPressed(';')) { gVol = gVol < 21 ? gVol + 1 : 21; setVol(); }
        if (keyPressed('.')) { gVol = gVol > 0 ? gVol - 1 : 0; setVol(); }
        if (keyPressed('m')) { gMute = !gMute; setVol(); }
        if (keyPressed('l')) { gList = true; gListSel = gCur; }
        if (keyPressed('r')) { playStation(); }
        if (keyPressed('b')) settingsSetBrightness((settingsBrightness() + 1) % 5);
    }

    if (now - gLastDraw >= 120) { gLastDraw = now; drawRadio(); }
    if (now - gLastMeter >= 50) { gLastMeter = now; if (!gList) drawMeter(); }
}
