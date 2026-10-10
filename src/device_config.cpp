#include "device_config.h"

#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <cstring>

#include "fpl_config.h"

#if __has_include("wifi_config.h")
#include "wifi_config.h"
#endif
#ifndef WIFI_SSID
#define WIFI_SSID ""
#endif
#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD ""
#endif

static constexpr const char *kPrefsNamespace = "fpldevice";
static constexpr const char *kKeySsid = "wifi_ssid";
static constexpr const char *kKeyPassword = "wifi_pass";
static constexpr const char *kKeyEntryId = "entry_id";
static constexpr const char *kKeyWebPassword = "web_pass";
static constexpr const char *kKeySetupMode = "setup_mode";

static SemaphoreHandle_t configMutex = nullptr;
static DeviceConfig config;

static bool lock() {
    return configMutex && xSemaphoreTake(configMutex, pdMS_TO_TICKS(200)) == pdTRUE;
}

static bool fits(const char *s, size_t capacity) {
    return s && strlen(s) < capacity;
}

void deviceConfigInit() {
    if (!configMutex) {
        configMutex = xSemaphoreCreateMutex();
    }
    strlcpy(config.wifiSsid, WIFI_SSID, sizeof(config.wifiSsid));
    strlcpy(config.wifiPassword, WIFI_PASSWORD, sizeof(config.wifiPassword));
    config.entryId = FPL_ENTRY_ID;
    config.webPassword[0] = '\0';

    Preferences prefs;
    if (prefs.begin(kPrefsNamespace, true)) {
        if (prefs.isKey(kKeySsid)) {
            prefs.getString(kKeySsid, config.wifiSsid, sizeof(config.wifiSsid));
            prefs.getString(kKeyPassword, config.wifiPassword, sizeof(config.wifiPassword));
        }
        const int entryId = prefs.getInt(kKeyEntryId, 0);
        if (entryId > 0) {
            config.entryId = entryId;
        }
        if (prefs.isKey(kKeyWebPassword)) {
            prefs.getString(kKeyWebPassword, config.webPassword, sizeof(config.webPassword));
        }
        prefs.end();
    }
    Serial.printf("[DEVICE] WiFi '%s', entry %d, web password %s\n", config.wifiSsid, config.entryId,
                  config.webPassword[0] ? "set" : "not set");
}

void deviceConfigGet(DeviceConfig &out) {
    if (!lock()) {
        return;
    }
    out = config;
    xSemaphoreGive(configMutex);
}

int deviceConfigEntryId() {
    if (!lock()) {
        return FPL_ENTRY_ID;
    }
    const int entryId = config.entryId;
    xSemaphoreGive(configMutex);
    return entryId;
}

bool deviceConfigSetWifi(const char *ssid, const char *password) {
    if (!fits(ssid, kWifiSsidLen) || !ssid[0] || !fits(password, kWifiPasswordLen) || !lock()) {
        return false;
    }
    strlcpy(config.wifiSsid, ssid, sizeof(config.wifiSsid));
    strlcpy(config.wifiPassword, password, sizeof(config.wifiPassword));
    Preferences prefs;
    if (prefs.begin(kPrefsNamespace, false)) {
        prefs.putString(kKeySsid, config.wifiSsid);
        prefs.putString(kKeyPassword, config.wifiPassword);
        prefs.end();
    }
    xSemaphoreGive(configMutex);
    return true;
}

bool deviceConfigSetEntryId(int entryId) {
    if (entryId <= 0 || !lock()) {
        return false;
    }
    config.entryId = entryId;
    Preferences prefs;
    if (prefs.begin(kPrefsNamespace, false)) {
        prefs.putInt(kKeyEntryId, entryId);
        prefs.end();
    }
    xSemaphoreGive(configMutex);
    return true;
}

bool deviceConfigSetWebPassword(const char *password) {
    if (!fits(password, kWebPasswordLen) || !lock()) {
        return false;
    }
    strlcpy(config.webPassword, password, sizeof(config.webPassword));
    Preferences prefs;
    if (prefs.begin(kPrefsNamespace, false)) {
        prefs.putString(kKeyWebPassword, config.webPassword);
        prefs.end();
    }
    xSemaphoreGive(configMutex);
    return true;
}

void deviceConfigRequestSetupMode(bool enabled) {
    Preferences prefs;
    if (prefs.begin(kPrefsNamespace, false)) {
        prefs.putBool(kKeySetupMode, enabled);
        prefs.end();
    }
}

bool deviceConfigSetupModeRequested() {
    Preferences prefs;
    if (!prefs.begin(kPrefsNamespace, true)) {
        return false;
    }
    const bool requested = prefs.getBool(kKeySetupMode, false);
    prefs.end();
    return requested;
}
