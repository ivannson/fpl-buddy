#pragma once

#include <Arduino.h>

// User-adjustable settings for the between-gameweeks screens and the LED ring. Defaults come from fpl_config.h; changes are
// made over serial (`screens help`) or the web UI and saved to NVS. Thread-safe: every accessor takes an internal mutex.

static constexpr size_t kMaxWatchlist = 8;
static constexpr size_t kWatchEntryLen = 24;
static constexpr size_t kMaxHiddenLeagues = 32;

struct DisplaySettings {
    uint16_t screenIntervalSec = 30;
    uint8_t rankGraphGameweeks = 5;  // 0 = all
    // Player web names (lower case) or element IDs, in display order.
    char watchlist[kMaxWatchlist][kWatchEntryLen] = {};
    size_t watchCount = 0;
    // Mini leagues are all shown unless their ID is here, so newly joined leagues appear automatically.
    int hiddenLeagueIds[kMaxHiddenLeagues] = {};
    size_t hiddenLeagueCount = 0;
    // LED ring. Brightness is a percentage (0-100) of FPL_LED_RING_MAX_BRIGHTNESS.
    uint8_t ledRankBrightnessPct = 100;
    uint8_t ledDeadlineBrightnessPct = 100;
    uint8_t ledNotificationBrightnessPct = 100;
    bool ledRankOnlyLiveGw = false;     // rank LEDs off between gameweeks
    bool ledRankOnlyMatchLive = false;  // rank LEDs off unless a match is in play
    uint32_t version = 0;          // bumped on every change
    uint32_t watchlistVersion = 0;  // bumped when the watchlist changes (needs a bootstrap refetch)
};

void displaySettingsInit();
void displaySettingsGet(DisplaySettings &out);
uint32_t displaySettingsVersion();

bool displaySettingsSetScreenInterval(uint16_t seconds);
bool displaySettingsSetRankGraphGameweeks(uint8_t gameweeks);
void displaySettingsReset();
// Replaces all user fields (interval, graph, watchlist, hidden leagues, LED ring) in one save. Watchlist entries are
// normalized and de-duplicated. Returns false (and changes nothing) if a value is invalid.
bool displaySettingsApply(const DisplaySettings &next);

enum class WatchlistResult { Ok, Full, Duplicate, NotFound, Invalid };
WatchlistResult displaySettingsWatchAdd(const char *entry);
WatchlistResult displaySettingsWatchRemove(const char *entry);
void displaySettingsWatchClear();

bool displaySettingsSetLeagueHidden(int leagueId, bool hidden);
void displaySettingsShowAllLeagues();
bool displaySettingsIsLeagueHidden(const DisplaySettings &settings, int leagueId);
