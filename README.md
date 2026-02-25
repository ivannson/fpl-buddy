# FPL Buddy — Waveshare ESP32-S3-Touch-LCD-1.46

Full build video coming soon on my YouTube channel: [Nvkvmakes](https://www.youtube.com/@Nvkvmakes).
Short-form project videos and work in progress will be posted on Instagram: [@nvkv.makes](https://www.instagram.com/nvkv.makes/).

FPL Buddy now targets the **Waveshare ESP32-S3-Touch-LCD-1.46** platform.

## Version Status

This is **v1** of the project.

- The current code works, but it still needs optimization (for example, avoiding frequent fetches of the large `bootstrap-static` endpoint).
- Planned feature ideas for screens that cycle between game weeks:
  - mini-league rank
  - price changes
  - predicted price changes
- CAD files and full assembly instructions will be published shortly.

## Hardware Migration Note

The project was migrated from Sunton ESP32-2424S012C (ESP32-C3) to **Waveshare ESP32-S3-Touch-LCD-1.46**.

### Why the board changed

- **Dual-core ESP32-S3**: enables splitting workloads (graphics/LVGL on one core, FPL API/network logic on the other).
- **More memory + PSRAM support**: improves stability for HTTPS + JSON parsing and larger display buffers.
- **Native 412x412 round display**: better fit for rich FPL dashboard visuals.

## Current App Behavior

- LVGL UI with animated full-screen arc and central points card.
- Wi-Fi connection + periodic FPL API polling.
- On-screen value shows **current gameweek points** (`summary_event_points`).

## Configuration

### FPL config

Edit `include/fpl_config.h`:

- `FPL_ENTRY_ID` - your FPL entry/team ID
- `FPL_POLL_INTERVAL_MS` - polling interval in milliseconds
- Optional tuning values (for example `FPL_ENABLE_NAME_LOOKUP`, LED ring settings, and JSON/PSRAM limits) can be adjusted for your hardware and preferred behavior.

Example:

```cpp
#pragma once

#define FPL_ENTRY_ID 1234567
#define FPL_POLL_INTERVAL_MS (60UL * 1000UL)

// Optional behavior flags
#define FPL_ENABLE_NAME_LOOKUP 1
#define FPL_USE_SERVER_EVENT_BREAKDOWN 1

// Optional LED ring settings
#define FPL_LED_RING_ENABLED 1
#define FPL_LED_RING_PIN 1
#define FPL_LED_RING_LED_COUNT 16
#define FPL_LED_RING_MAX_BRIGHTNESS 64
```

### Wi-Fi config

`include/wifi_config.h` is intentionally ignored by Git (to avoid committing credentials).

Create it locally with:

```cpp
#pragma once

#define WIFI_SSID     "YOUR_WIFI_NAME"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"
```

Do not commit this file with real credentials.

After creating it, the firmware reads:

- `WIFI_SSID`
- `WIFI_PASSWORD`

## Installation

1. Install [PlatformIO Core](https://docs.platformio.org/en/latest/core/installation/index.html) (or use PlatformIO in VS Code).
2. Clone this repository:

```bash
git clone https://github.com/ivannson/fpl-buddy.git
cd fpl-buddy
```

3. Pull required submodules (including custom board definitions):

```bash
git submodule update --init --recursive
```

4. Create/update required local configuration files:
- Create `include/wifi_config.h` (template above) with your Wi-Fi credentials.
- Edit `include/fpl_config.h` with your FPL team/entry ID (`FPL_ENTRY_ID`).
- If your wiring differs, review `platformio.ini` and LED ring settings in `include/fpl_config.h` (pin, LED count, brightness).

5. Build and flash:

```bash
pio run
pio run -t upload
pio device monitor
```

6. Upload kit images to filesystem (separate from firmware):

```bash
pio run -t uploadfs
```

This uploads the `data/` folder (including `data/kits/*.rgb565`) to LittleFS on the ESP32.

If kits are not uploaded, event notifications still work, but they will display without shirt images.

Example update workflow:

```bash
# Code only
pio run -t upload

# Kits/filesystem only
pio run -t uploadfs
```

## PlatformIO target

Active environment in `platformio.ini`:

- `esp32-s3-touch-lcd-146`

Key display flags:

- `DISPLAY_WIDTH=412`
- `DISPLAY_HEIGHT=412`

## Notes

- Display driver path uses `lib/SPD2010/*` (QSPI panel + touch integration).
- If you previously used Sunton docs/settings, treat them as legacy; this repository now documents and supports the Waveshare ESP32-S3 board.
