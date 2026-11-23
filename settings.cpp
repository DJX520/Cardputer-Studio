// settings.cpp — persistent settings (theme/brightness), a WiFi manager, and
// the system settings app.
#include "settings.h"
#include "config.h"
#include "theme.h"
#include "keyboard.h"
#include <M5Unified.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_wifi.h>

// ---- persistent settings --------------------------------------------------
static Preferences prefs;
static int    gTheme = 0;
static uint8_t gBright = 4;

void settingsLoad() {
    prefs.begin("studio", false);
    gTheme = prefs.getInt("theme", 0);
    gBright = prefs.getUChar("bright", 4);
    prefs.end();
    M5.Display.setBrightness(gBright == 0 ? 8 : gBright * 51 > 255 ? 255 : gBright * 51);
}
int  settingsTheme() { return gTheme; }
void settingsSetTheme(int i) { gTheme = (i < 0 || i >= 6) ? 0 : i; prefs.begin("studio", false); prefs.putInt("theme", gTheme); prefs.end(); }
uint8_t settingsBrightness() { return gBright; }
void settingsSetBrightness(uint8_t b) {
    gBright = b > 4 ? 4 : b;
    prefs.begin("studio", false); prefs.putUChar("bright", gBright); prefs.end();
    M5.Display.setBrightness(gBright == 0 ? 8 : gBright * 51 > 255 ? 255 : gBright * 51);
}

// ---- text input -----------------------------------------------------------
static char typedChar() {
    static const char* lower = "abcdefghijklmnopqrstuvwxyz";
    static const char* digits = "1234567890";
    const bool fn = keyHeld(K_FN);
    for (const char* p = lower; *p; p++) if (keyPressed(*p)) return fn ? (char)(*p - 'a' + 'A') : *p;
    for (const char* p = digits; *p; p++) if (keyPressed(*p)) return *p;
    static const char* sym = "`-=[]\\;',./ ";
    for (const char* p = sym; *p; p++) if (keyPressed(*p)) return *p;
    return 0;
}

static String inputText(const char* prompt, bool isPassword) {
    String s; s.reserve(64);
    M5.Display.fillScreen(themeAt(gTheme).bg);
    M5.Display.setFont(&fonts::Font2);
    M5.Display.setTextColor(themeAt(gTheme).ink, themeAt(gTheme).bg);
    M5.Display.drawString(prompt, 4, 8);
    while (true) {
        keyboardPoll();
        char c = typedChar();
        if (c) { if (s.length() < 63) s += c; }
        if (keyPressed(K_BKSP)) { if (s.length()) s.remove(s.length() - 1); }
        if (keyPressed(K_ENTER)) break;
        if (keyPressed(K_TAB)) { s = ""; break; }
        String disp = "> ";
        for (size_t i = 0; i < s.length(); i++) disp += isPassword ? '*' : s[i];
        M5.Display.fillRect(0, 30, 240, 20, themeAt(gTheme).bg);
        M5.Display.drawString(disp, 4, 30);
        delay(8);
    }
    return s;
}

// ---- WiFi manager (stores up to 5 networks in Preferences) ----------------
#define MAX_SAVED_WIFI 5
static String gSsid[MAX_SAVED_WIFI], gPass[MAX_SAVED_WIFI];
static int gSavedCount = 0, gLastUsed = -1;

static void loadSavedWifi() {
    prefs.begin("wifi", true);
    gSavedCount = 0;
    gLastUsed = prefs.getInt("last", -1);
    for (int i = 0; i < MAX_SAVED_WIFI; i++) {
        String ss = prefs.getString(("s" + String(i)).c_str(), "");
        if (ss.length()) { gSsid[gSavedCount] = ss; gPass[gSavedCount] = prefs.getString(("p" + String(i)).c_str(), ""); gSavedCount++; }
    }
    prefs.end();
}

static void saveWifi(int idx, const String& ssid, const String& pass) {
    prefs.begin("wifi", false);
    prefs.putString(("s" + String(idx)).c_str(), ssid);
    prefs.putString(("p" + String(idx)).c_str(), pass);
    prefs.putInt("last", idx);
    prefs.end();
}

static void resetWifi() {
    prefs.begin("wifi", false);
    for (int i = 0; i < MAX_SAVED_WIFI; i++) { prefs.remove(("s" + String(i)).c_str()); prefs.remove(("p" + String(i)).c_str()); }
    prefs.remove("last"); prefs.end();
    WiFi.disconnect(true, true);
    gSavedCount = 0; gLastUsed = -1;
}

static bool tryConnect(const String& ssid, const String& pass) {
    WiFi.disconnect(true, true);
    WiFi.begin(ssid.c_str(), pass.c_str());
    const unsigned long t0 = millis();
    while (millis() - t0 < 10000) {
        keyboardPoll();
        if (WiFi.status() == WL_CONNECTED) return true;
        M5.Display.fillRect(0, 60, 240, 16, themeAt(gTheme).bg);
        M5.Display.drawString("Connecting...", 4, 60);
        delay(20);
    }
    WiFi.disconnect(true, true);
    return false;
}

static void connectToWifi() {
    loadSavedWifi();
    if (gSavedCount > 0 && gLastUsed >= 0 && gLastUsed < gSavedCount)
        if (tryConnect(gSsid[gLastUsed], gPass[gLastUsed])) return;
    for (int i = 0; i < gSavedCount; i++)
        if (tryConnect(gSsid[i], gPass[i])) { gLastUsed = i; prefs.begin("wifi", false); prefs.putInt("last", i); prefs.end(); return; }
    // no saved network: ask for SSID + password
    M5.Display.fillScreen(themeAt(gTheme).bg);
    String ssid = inputText("SSID:", false);
    if (!ssid.length()) return;
    String pass = inputText("Password:", true);
    int idx = gSavedCount < MAX_SAVED_WIFI ? gSavedCount : MAX_SAVED_WIFI - 1;
    if (gSavedCount < MAX_SAVED_WIFI) gSavedCount++;
    saveWifi(idx, ssid, pass);
    tryConnect(ssid, pass);
}

// ---- settings UI ----------------------------------------------------------
static const char* kItems[6] = {"Theme", "Brightness", "WiFi connect", "Reset WiFi", "About", "Back"};
static const int kItemN = 6;
static int gSel = 0;
static bool gInWifi = false;

static void drawSettings() {
    const Theme& t = themeAt(gTheme);
    M5.Display.fillScreen(t.bg);
    M5.Display.setFont(&fonts::Font2);
    M5.Display.setTextColor(t.accent, t.bg);
    M5.Display.drawString("SETTINGS", 2, 2);
    for (int i = 0; i < kItemN; i++) {
        const int y = 22 + i * 18;
        const bool sel = i == gSel;
        if (sel) M5.Display.fillRect(0, y - 2, 240, 16, t.panelHi);
        M5.Display.setTextColor(sel ? t.ink : t.dim, sel ? t.panelHi : t.bg);
        char b[40];
        if (i == 0) snprintf(b, sizeof(b), "%s", themeName(gTheme));
        else if (i == 1) snprintf(b, sizeof(b), "%d", (int)gBright);
        else if (i == 4) snprintf(b, sizeof(b), "Cardputer Studio v1.0");
        else snprintf(b, sizeof(b), "%s", kItems[i]);
        M5.Display.drawString(b, 4, y);
    }
    M5.Display.setFont(&fonts::Font0);
    M5.Display.setTextColor(t.dim, t.bg);
    M5.Display.drawString(";/. move  enter ok  tab back", 2, 128);
}

void settingsAppBegin() {
    gSel = 0; gInWifi = false;
    M5.Display.setRotation(1);
    drawSettings();
}

void settingsAppLoop() {
    if (gInWifi) { gInWifi = false; connectToWifi(); drawSettings(); return; }
    static uint32_t lastDraw = 0;
    if (keyPressed(';')) { gSel = (gSel - 1 + kItemN) % kItemN; drawSettings(); }
    if (keyPressed('.')) { gSel = (gSel + 1) % kItemN; drawSettings(); }
    if (keyPressed(K_TAB)) { /* leave handled by launcher */ }
    if (keyPressed(K_ENTER)) {
        switch (gSel) {
            case 0: settingsSetTheme((gTheme + 1) % 6); break;
            case 1: settingsSetBrightness((gBright + 1) % 5); break;
            case 2: gInWifi = true; break;
            case 3: resetWifi(); break;
            case 4: break;
            case 5: break;
        }
        drawSettings();
    }
    if (millis() - lastDraw > 100) { lastDraw = millis(); }
}

void settingsAppStop() {}

bool wifiEnsureConnected() {
    if (WiFi.status() == WL_CONNECTED) return true;
    connectToWifi();
    return WiFi.status() == WL_CONNECTED;
}
