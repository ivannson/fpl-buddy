#pragma once

#include <Arduino.h>

// Device-level settings (WiFi, FPL entry ID, web UI password), saved to NVS. First-boot defaults come from
// wifi_config.h (optional) and fpl_config.h. Changed from the web UI (web_server.cpp). Thread-safe.

static constexpr size_t kWifiSsidLen = 33;      // 32 chars + NUL
static constexpr size_t kWifiPasswordLen = 65;  // 64 chars + NUL
static constexpr size_t kWebPasswordLen = 33;

struct DeviceConfig {
    char wifiSsid[kWifiSsidLen] = "";
    char wifiPassword[kWifiPasswordLen] = "";
    int entryId = 0;
    char webPassword[kWebPasswordLen] = "";  // empty = web UI open to the local network
};

void deviceConfigInit();
void deviceConfigGet(DeviceConfig &out);
int deviceConfigEntryId();

// Each returns false if the value is invalid; valid values are saved to NVS.
bool deviceConfigSetWifi(const char *ssid, const char *password);
bool deviceConfigSetEntryId(int entryId);
bool deviceConfigSetWebPassword(const char *password);

// Setup mode (WiFi access point + captive portal) on next boot, even if WiFi credentials are saved.
void deviceConfigRequestSetupMode(bool enabled);
bool deviceConfigSetupModeRequested();
