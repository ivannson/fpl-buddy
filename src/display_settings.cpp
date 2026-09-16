#include "display_settings.h"

#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <cctype>
#include <cstring>

#include "fpl_config.h"

static constexpr const char *kPrefsNamespace = "fplbuddy";
static constexpr const char *kKeyInterval = "scr_int";
static constexpr const char *kKeyGraph = "graph_gw";
static constexpr const char *kKeyWatchlist = "watch";
static constexpr const char *kKeyHiddenLeagues = "lg_hidden";

static SemaphoreHandle_t settingsMutex = nullptr;
static DisplaySettings settings;

static bool isValidGraphGameweeks(uint8_t gameweeks) {
    return gameweeks == 0 || gameweeks == 3 || gameweeks == 5 || gameweeks == 10;
}

static bool isValidInterval(uint16_t seconds) {
    return seconds >= 5 && seconds <= 600;
}

static void normalizeEntry(const char *in, char *out, size_t outLen) {
    while (*in == ' ') {
        ++in;
    }
    size_t n = 0;
    for (; in[n] && n + 1 < outLen; ++n) {
        out[n] = static_cast<char>(tolower(static_cast<unsigned char>(in[n])));
    }
    while (n > 0 && out[n - 1] == ' ') {
        --n;
    }
    out[n] = '\0';
}

// Comma-separated list, e.g. "Saka, 427" -> {"saka", "427"}.
static void parseWatchlist(const char *csv, DisplaySettings &s) {
    s.watchCount = 0;
    char token[kWatchEntryLen];
    size_t len = 0;
    for (const char *p = csv;; ++p) {
        if (*p == ',' || *p == '\0') {
            token[len] = '\0';
            char entry[kWatchEntryLen];
            normalizeEntry(token, entry, sizeof(entry));
            if (entry[0] && s.watchCount < kMaxWatchlist) {
                strlcpy(s.watchlist[s.watchCount++], entry, kWatchEntryLen);
            }
            len = 0;
            if (*p == '\0') {
                break;
            }
        } else if (len + 1 < sizeof(token)) {
            token[len++] = *p;
        }
    }
}

static void saveLocked() {
    Preferences prefs;
    if (!prefs.begin(kPrefsNamespace, false)) {
        Serial.println("[SETTINGS] NVS open failed, settings not saved");
        return;
    }
    prefs.putUShort(kKeyInterval, settings.screenIntervalSec);
    prefs.putUChar(kKeyGraph, settings.rankGraphGameweeks);
    String csv;
    for (size_t i = 0; i < settings.watchCount; ++i) {
        if (i) {
            csv += ",";
        }
        csv += settings.watchlist[i];
    }
    prefs.putString(kKeyWatchlist, csv);
    prefs.putBytes(kKeyHiddenLeagues, settings.hiddenLeagueIds, settings.hiddenLeagueCount * sizeof(int));
    prefs.end();
}

static void applyDefaultsLocked() {
    settings.screenIntervalSec = FPL_IDLE_SCREEN_INTERVAL_SEC;
    settings.rankGraphGameweeks = FPL_RANK_GRAPH_GAMEWEEKS;
    parseWatchlist(FPL_DEFAULT_WATCHLIST, settings);
    settings.hiddenLeagueCount = 0;
}

static bool lock() {
    return settingsMutex && xSemaphoreTake(settingsMutex, pdMS_TO_TICKS(200)) == pdTRUE;
}

static void commitLocked(bool watchlistChanged) {
    settings.version++;
    if (watchlistChanged) {
        settings.watchlistVersion++;
    }
    saveLocked();
    xSemaphoreGive(settingsMutex);
}

void displaySettingsInit() {
    if (!settingsMutex) {
        settingsMutex = xSemaphoreCreateMutex();
    }
    applyDefaultsLocked();

    Preferences prefs;
    if (prefs.begin(kPrefsNamespace, true)) {
        const uint16_t interval = prefs.getUShort(kKeyInterval, settings.screenIntervalSec);
        if (isValidInterval(interval)) {
            settings.screenIntervalSec = interval;
        }
        const uint8_t graph = prefs.getUChar(kKeyGraph, settings.rankGraphGameweeks);
        if (isValidGraphGameweeks(graph)) {
            settings.rankGraphGameweeks = graph;
        }
        // A saved empty string means the user cleared the watchlist, so only a missing key keeps the default.
        if (prefs.isKey(kKeyWatchlist)) {
            parseWatchlist(prefs.getString(kKeyWatchlist, "").c_str(), settings);
        }
        const size_t hiddenBytes = prefs.getBytesLength(kKeyHiddenLeagues);
        if (hiddenBytes > 0 && hiddenBytes <= sizeof(settings.hiddenLeagueIds) && hiddenBytes % sizeof(int) == 0) {
            prefs.getBytes(kKeyHiddenLeagues, settings.hiddenLeagueIds, hiddenBytes);
            settings.hiddenLeagueCount = hiddenBytes / sizeof(int);
        }
        prefs.end();
    }
    settings.version = 1;
    settings.watchlistVersion = 1;

    Serial.printf("[SETTINGS] screen interval %us, rank graph %u GWs, %u watched, %u hidden leagues\n",
                  settings.screenIntervalSec, settings.rankGraphGameweeks, static_cast<unsigned>(settings.watchCount),
                  static_cast<unsigned>(settings.hiddenLeagueCount));
}

void displaySettingsGet(DisplaySettings &out) {
    if (!lock()) {
        return;
    }
    out = settings;
    xSemaphoreGive(settingsMutex);
}

uint32_t displaySettingsVersion() {
    if (!lock()) {
        return 0;
    }
    const uint32_t version = settings.version;
    xSemaphoreGive(settingsMutex);
    return version;
}

bool displaySettingsSetScreenInterval(uint16_t seconds) {
    if (!isValidInterval(seconds) || !lock()) {
        return false;
    }
    settings.screenIntervalSec = seconds;
    commitLocked(false);
    return true;
}

bool displaySettingsSetRankGraphGameweeks(uint8_t gameweeks) {
    if (!isValidGraphGameweeks(gameweeks) || !lock()) {
        return false;
    }
    settings.rankGraphGameweeks = gameweeks;
    commitLocked(false);
    return true;
}

void displaySettingsReset() {
    if (!lock()) {
        return;
    }
    applyDefaultsLocked();
    commitLocked(true);
}

WatchlistResult displaySettingsWatchAdd(const char *entry) {
    char normalized[kWatchEntryLen];
    normalizeEntry(entry ? entry : "", normalized, sizeof(normalized));
    if (!normalized[0] || strchr(normalized, ',')) {
        return WatchlistResult::Invalid;
    }
    if (!lock()) {
        return WatchlistResult::Invalid;
    }
    for (size_t i = 0; i < settings.watchCount; ++i) {
        if (strcmp(settings.watchlist[i], normalized) == 0) {
            xSemaphoreGive(settingsMutex);
            return WatchlistResult::Duplicate;
        }
    }
    if (settings.watchCount >= kMaxWatchlist) {
        xSemaphoreGive(settingsMutex);
        return WatchlistResult::Full;
    }
    strlcpy(settings.watchlist[settings.watchCount++], normalized, kWatchEntryLen);
    commitLocked(true);
    return WatchlistResult::Ok;
}

WatchlistResult displaySettingsWatchRemove(const char *entry) {
    char normalized[kWatchEntryLen];
    normalizeEntry(entry ? entry : "", normalized, sizeof(normalized));
    if (!lock()) {
        return WatchlistResult::Invalid;
    }
    for (size_t i = 0; i < settings.watchCount; ++i) {
        if (strcmp(settings.watchlist[i], normalized) != 0) {
            continue;
        }
        for (size_t j = i + 1; j < settings.watchCount; ++j) {
            strlcpy(settings.watchlist[j - 1], settings.watchlist[j], kWatchEntryLen);
        }
        settings.watchCount--;
        commitLocked(true);
        return WatchlistResult::Ok;
    }
    xSemaphoreGive(settingsMutex);
    return WatchlistResult::NotFound;
}

void displaySettingsWatchClear() {
    if (!lock()) {
        return;
    }
    settings.watchCount = 0;
    commitLocked(true);
}

bool displaySettingsIsLeagueHidden(const DisplaySettings &s, int leagueId) {
    for (size_t i = 0; i < s.hiddenLeagueCount; ++i) {
        if (s.hiddenLeagueIds[i] == leagueId) {
            return true;
        }
    }
    return false;
}

bool displaySettingsSetLeagueHidden(int leagueId, bool hidden) {
    if (leagueId <= 0 || !lock()) {
        return false;
    }
    size_t index = settings.hiddenLeagueCount;
    for (size_t i = 0; i < settings.hiddenLeagueCount; ++i) {
        if (settings.hiddenLeagueIds[i] == leagueId) {
            index = i;
            break;
        }
    }
    const bool isHidden = index < settings.hiddenLeagueCount;
    if (hidden == isHidden) {
        xSemaphoreGive(settingsMutex);
        return true;
    }
    if (hidden) {
        if (settings.hiddenLeagueCount >= kMaxHiddenLeagues) {
            xSemaphoreGive(settingsMutex);
            return false;
        }
        settings.hiddenLeagueIds[settings.hiddenLeagueCount++] = leagueId;
    } else {
        settings.hiddenLeagueIds[index] = settings.hiddenLeagueIds[--settings.hiddenLeagueCount];
    }
    commitLocked(false);
    return true;
}

void displaySettingsShowAllLeagues() {
    if (!lock()) {
        return;
    }
    settings.hiddenLeagueCount = 0;
    commitLocked(false);
}
