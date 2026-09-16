#pragma once

#include <Arduino.h>

// User-adjustable settings for the between-gameweeks screens. Defaults come from fpl_config.h; changes are
// made over serial (`screens help`) and saved to NVS. Thread-safe: every accessor takes an internal mutex.

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
    uint32_t version = 0;           // bumped on every change
    uint32_t watchlistVersion = 0;  // bumped when the watchlist changes (needs a bootstrap refetch)
};

void displaySettingsInit();
void displaySettingsGet(DisplaySettings &out);
uint32_t displaySettingsVersion();

bool displaySettingsSetScreenInterval(uint16_t seconds);
bool displaySettingsSetRankGraphGameweeks(uint8_t gameweeks);
void displaySettingsReset();

enum class WatchlistResult { Ok, Full, Duplicate, NotFound, Invalid };
WatchlistResult displaySettingsWatchAdd(const char *entry);
WatchlistResult displaySettingsWatchRemove(const char *entry);
void displaySettingsWatchClear();

bool displaySettingsSetLeagueHidden(int leagueId, bool hidden);
void displaySettingsShowAllLeagues();
bool displaySettingsIsLeagueHidden(const DisplaySettings &settings, int leagueId);
