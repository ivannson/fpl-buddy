# FPL Buddy — Waveshare ESP32-S3-Touch-LCD-1.46

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

### Wi-Fi config

Edit `include/wifi_config.h`:

- `WIFI_SSID`
- `WIFI_PASSWORD`

## Installation

1. Install [PlatformIO Core](https://docs.platformio.org/en/latest/core/installation/index.html) (or use PlatformIO in VS Code).
2. Clone this repository:

```bash
git clone https://github.com/ivannson/fpl-buddy.git
cd fpl-buddy
```

3. Update project configuration:
- `include/wifi_config.h` with your Wi-Fi credentials
- `include/fpl_config.h` with your FPL team/entry ID

4. Build and flash:

```bash
pio run
pio run -t upload
pio device monitor
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
