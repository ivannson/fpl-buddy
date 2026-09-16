# FPL API tests

Python scripts to check the FPL API against what the firmware in `src/main.cpp` expects, and to test data for new features.
Output files are written to `outputs/`, which git ignores.

Requires `requests` (already in the PlatformIO penv). The entry ID defaults to `FPL_ENTRY_ID` from `include/fpl_config.h`. Override it with an argument or the `FPL_ENTRY_ID` env var.

| Script | What it does |
|---|---|
| `fetch_all.py [entry]` | Saves raw JSON from every endpoint the firmware uses, plus candidate endpoints, to `outputs/raw/` |
| `schema_dump.py` | Lists every key, type and example value from `outputs/raw/` in `outputs/schema.txt` |
| `check_firmware_fields.py` | Checks that each field the firmware reads still exists with the right type, that every explain identifier is handled, and that each team maps to a kit file |
| `price_changes.py snapshot [entry]` / `verify` | Saves a snapshot of price-change data. `verify` compares the last two snapshots to see which projections came true |
| `mini_league.py [entry]` | Shows league ranks from `entry/{id}/` and full private-league tables |
| `live_watch.py [entry] [interval_s] [minutes]` | Run during a live gameweek. Logs which fields change and when |

Typical flow: `fetch_all.py` → `check_firmware_fields.py`. Run `price_changes.py snapshot` before and after a price deadline (23:00 UTC), then run `verify`. Run `live_watch.py` while matches are in progress.
