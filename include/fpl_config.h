#pragma once

// Firmware version, shown in the web UI (http://fpl-buddy.local). Bump it for each release.
#define FPL_FIRMWARE_VERSION "1.0.0"

// Your public FPL entry/team ID (first-boot default; change it later in the web UI):
// https://fantasy.premierleague.com/entry/<ID>/event/<GW>
#ifndef FPL_ENTRY_ID
#define FPL_ENTRY_ID 2752740
#endif

// Poll interval while a match is in play (and the minimum gap between polls).
#ifndef FPL_POLL_INTERVAL_MS
#define FPL_POLL_INTERVAL_MS (60UL * 1000UL)
#endif

// Poll interval when no match is in play. Polls still wake early for the next kickoff or deadline.
#ifndef FPL_IDLE_POLL_INTERVAL_MS
#define FPL_IDLE_POLL_INTERVAL_MS (10UL * 60UL * 1000UL)
#endif

// Max age of cached `bootstrap-static` data (gameweek flags, deadline, player names). It is also refetched
// on GW change, after the deadline passes, and when the squad has unknown players.
#ifndef FPL_BOOTSTRAP_REFRESH_MS
#define FPL_BOOTSTRAP_REFRESH_MS (30UL * 60UL * 1000UL)
#endif

// Max age of cached picks (auto-subs change multipliers after matches).
#ifndef FPL_PICKS_REFRESH_MS
#define FPL_PICKS_REFRESH_MS (5UL * 60UL * 1000UL)
#endif

// Max age of cached fixtures for the current GW (kickoff times, finished flags).
#ifndef FPL_FIXTURES_REFRESH_MS
#define FPL_FIXTURES_REFRESH_MS (10UL * 60UL * 1000UL)
#endif

// Max age of `/event/{gw}/live/` data when no match is in play (catches late corrections).
#ifndef FPL_LIVE_IDLE_REFRESH_MS
#define FPL_LIVE_IDLE_REFRESH_MS (30UL * 60UL * 1000UL)
#endif

// Max age of `entry/{id}/history/` (overall rank per gameweek, for the rank graph).
#ifndef FPL_HISTORY_REFRESH_MS
#define FPL_HISTORY_REFRESH_MS (60UL * 60UL * 1000UL)
#endif

// --- Between-gameweeks screens ---------------------------------------------------------------------------
// While no gameweek is live, the display cycles through: rank, rank graph, squad price changes, watchlist,
// mini leagues. Tap to skip ahead. These are first-boot defaults: change them over serial (`screens help`,
// `watch ...`, `league ...`); changes are saved to flash and override these values.

// Seconds per screen (5-600).
#ifndef FPL_IDLE_SCREEN_INTERVAL_SEC
#define FPL_IDLE_SCREEN_INTERVAL_SEC 30
#endif

// Gameweeks on the rank graph: 3, 5, 10, or 0 for all.
#ifndef FPL_RANK_GRAPH_GAMEWEEKS
#define FPL_RANK_GRAPH_GAMEWEEKS 5
#endif

// Watched players for price changes: comma-separated web names (as on the FPL site) or element IDs.
// Up to 8. Empty string = no watchlist screen.
#ifndef FPL_DEFAULT_WATCHLIST
#define FPL_DEFAULT_WATCHLIST "Saka"
#endif

// A price change is expected when the projected progress for the next price update
// (`price_change_projections[0].projected_percent`) reaches +/- this value.
#ifndef FPL_PRICE_CHANGE_THRESHOLD_PCT
#define FPL_PRICE_CHANGE_THRESHOLD_PCT 100
#endif

// `bootstrap-static` is large and can be unreliable on-device.
// Keep disabled for stable polling; enable only when name lookup is needed.
#ifndef FPL_ENABLE_NAME_LOOKUP
#define FPL_ENABLE_NAME_LOOKUP 1
#endif

// Upper bound for /bootstrap-static payload stored in PSRAM before parsing.
#ifndef FPL_BOOTSTRAP_PSRAM_MAX_BYTES
#define FPL_BOOTSTRAP_PSRAM_MAX_BYTES (3UL * 1024UL * 1024UL)
#endif

// ArduinoJson document capacity for the filtered /bootstrap-static (all players with name, team and price
// projections is ~200 KB).
#ifndef FPL_BOOTSTRAP_JSON_DOC_CAPACITY
#define FPL_BOOTSTRAP_JSON_DOC_CAPACITY 320000UL
#endif

// Upper bound for /event/{gw}/live/ payload stored in PSRAM before parsing.
#ifndef FPL_LIVE_PSRAM_MAX_BYTES
#define FPL_LIVE_PSRAM_MAX_BYTES (2UL * 1024UL * 1024UL)
#endif

// ArduinoJson document capacity for parsing /event/{gw}/live/ payload.
// Increase when using server `explain` breakdown to avoid NoMemory.
#ifndef FPL_LIVE_JSON_DOC_CAPACITY
#define FPL_LIVE_JSON_DOC_CAPACITY 700000UL
#endif

// Notification source:
// 1 = use server event breakdown (`/event/{gw}/live` -> `explain`)
// 0 = use inferred local logic from stat deltas
#ifndef FPL_USE_SERVER_EVENT_BREAKDOWN
#define FPL_USE_SERVER_EVENT_BREAKDOWN 1
#endif

// 16-LED WS2812/NeoPixel status ring.
#ifndef FPL_LED_RING_ENABLED
#define FPL_LED_RING_ENABLED 1
#endif

#ifndef FPL_LED_RING_PIN
#define FPL_LED_RING_PIN 13
#endif

#ifndef FPL_LED_RING_LED_COUNT
#define FPL_LED_RING_LED_COUNT 16
#endif

// Hardware cap (0-255). The brightness settings below are a percentage of this.
#ifndef FPL_LED_RING_MAX_BRIGHTNESS
#define FPL_LED_RING_MAX_BRIGHTNESS 64
#endif

// First-boot defaults for the LED ring settings (web UI), saved to flash once changed.
// Brightness in percent of FPL_LED_RING_MAX_BRIGHTNESS (0 = off).
#ifndef FPL_LED_RANK_BRIGHTNESS_PCT
#define FPL_LED_RANK_BRIGHTNESS_PCT 100
#endif

#ifndef FPL_LED_DEADLINE_BRIGHTNESS_PCT
#define FPL_LED_DEADLINE_BRIGHTNESS_PCT 100
#endif

// Purple flash when a player event pops up.
#ifndef FPL_LED_NOTIFICATION_BRIGHTNESS_PCT
#define FPL_LED_NOTIFICATION_BRIGHTNESS_PCT 100
#endif

// 1 = rank LEDs only while the gameweek is live (not between gameweeks).
#ifndef FPL_LED_RANK_ONLY_LIVE_GW
#define FPL_LED_RANK_ONLY_LIVE_GW 0
#endif

// 1 = rank LEDs only while a match is in play.
#ifndef FPL_LED_RANK_ONLY_MATCH_LIVE
#define FPL_LED_RANK_ONLY_MATCH_LIVE 0
#endif

#ifndef FPL_LED_RING_SPIN_INTERVAL_MS
#define FPL_LED_RING_SPIN_INTERVAL_MS 90U
#endif

#ifndef FPL_LED_RING_PULSE_PERIOD_MS
#define FPL_LED_RING_PULSE_PERIOD_MS 1400U
#endif

#ifndef FPL_LED_RING_NOTIFICATION_FLASH_COUNT
#define FPL_LED_RING_NOTIFICATION_FLASH_COUNT 2U
#endif

#ifndef FPL_LED_RING_NOTIFICATION_FLASH_MS
#define FPL_LED_RING_NOTIFICATION_FLASH_MS 160U
#endif

// Animation modes:
// 0 = breathing (all LEDs pulse together)
// 1 = dim-notch (all lit, one dim LED rotates)
// 2 = comet (bright head + fading tail rotates)
#ifndef FPL_LED_RING_DEFAULT_ANIMATION
#define FPL_LED_RING_DEFAULT_ANIMATION 2
#endif
