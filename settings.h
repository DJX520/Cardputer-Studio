// settings.h — persistent settings (theme/brightness), WiFi manager, and the
// system settings app (one of the three launcher modes).
#pragma once
#include <cstdint>

void settingsLoad();
int    settingsTheme();
void   settingsSetTheme(int i);
uint8_t settingsBrightness();
void   settingsSetBrightness(uint8_t b);

// settings app
void settingsAppBegin();
void settingsAppLoop();
void settingsAppStop();

// WiFi helper for the radio (ensures a connection; returns true when connected)
bool wifiEnsureConnected();
