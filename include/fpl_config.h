#pragma once

// Your public FPL entry/team ID:
// https://fantasy.premierleague.com/entry/<ID>/event/<GW>
#ifndef FPL_ENTRY_ID
#define FPL_ENTRY_ID 2910482
#endif

#ifndef FPL_POLL_INTERVAL_MS
#define FPL_POLL_INTERVAL_MS (60UL * 1000UL)
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

// Upper bound for /event/{gw}/live/ payload stored in PSRAM before parsing.
#ifndef FPL_LIVE_PSRAM_MAX_BYTES
#define FPL_LIVE_PSRAM_MAX_BYTES (2UL * 1024UL * 1024UL)
#endif
