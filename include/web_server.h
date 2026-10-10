#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

// Web UI on port 80 for display settings, device config (WiFi, entry ID, password) and firmware upload.
// Runs in its own task. In station mode it is reachable at http://fpl-buddy.local; in setup mode the device
// is a WiFi access point and every DNS lookup points at the UI (captive portal).

static constexpr const char *kWebHostname = "fpl-buddy";

// Starts the server task. Safe to call more than once; only the first call has an effect.
void webServerStart(bool setupMode);
bool webServerOtaInProgress();
// Unused bytes of the server task's stack at its fullest so far, or -1 if not started.
int webServerStackHighWaterMark();

// Hooks implemented in main.cpp.
struct WebLeagueInfo {
    int id = 0;
    char name[40] = "";
};
// Returns false while there is no FPL data yet.
bool appGetMiniLeagues(WebLeagueInfo *out, size_t capacity, size_t &count);
void appRequestFplPoll();
void appShowStatus(const char *text, uint32_t colorHex);
// Demo mode (the web UI's Demo tab): the demo state as JSON, and commands ({"action": ...}). A command
// returns false and sets `error` when it is rejected.
void appDemoStateJson(String &out);
bool appDemoCommand(JsonObjectConst cmd, const char *&error);
