# FPL Buddy — ESP32-2424S012C Display Test

LVGL display validation for the Sunton ESP32-2424S012C (round CYD board). Proves the
toolchain, smartdisplay library, LVGL rendering, and capacitive touch all work before
building the full FPL desk application.

---

## Hardware

### Board: ESP32-2424S012C (Sunton CYD)

| Spec | Detail |
|---|---|
| CPU | ESP32-C3-MINI-1U — single-core RISC-V @ 160 MHz |
| Flash | 4 MB, no PSRAM |
| Display | 240×240 round IPS, **GC9A01A** controller, SPI |
| Touch | **CST816S** capacitive, I2C |
| USB | USB-C with native USB serial (CDC) |

### Pin map

| Function | GPIO | Notes |
|---|---|---|
| SPI MOSI | 7 | Display data |
| SPI SCLK | 6 | Display clock |
| Display CS | 10 | Chip-select, active low |
| Display DC | 2 | Data/command select |
| Backlight | 3 | PWM-controlled |
| I2C SDA | 4 | Touch controller |
| I2C SCL | 5 | Touch controller |
| Touch RST | 1 | CST816S reset |
| Touch INT | 0 | CST816S interrupt |
| Boot button | 9 | Hold for bootloader entry |

All of these are declared in `boards/esp32-2424S012C.json` (see *Board definition codes* below).

---

## What the test displays

| Element | What it proves |
|---|---|
| Orange animated arc | LVGL widget rendering + animation system |
| White circle | Basic shape / placeholder for future custom graphics |
| "LVGL OK" label | Font rendering |
| Serial touch log | CST816S capacitive touch + LVGL input device |

---

## Project layout

```
fpl-buddy/
├── platformio.ini              # Build config — see "platformio.ini explained" below
├── boards/                     # Sunton board definitions (git submodule)
│   └── esp32-2424S012C.json    # Pin map + display + touch defines for this board
├── include/
│   ├── lv_conf.h               # LVGL v9 configuration (copied from template, enabled)
│   └── test_image.h            # Stub — will hold converted image assets later
├── src/
│   └── main.cpp                # Initialisation, UI setup, touch callback
├── lib/                        # Empty — project-local libraries go here if needed
└── test/                       # Empty — unit tests go here if needed
```

---

## Quick start

```bash
git clone --recurse-submodules https://github.com/ivannson/fpl-buddy.git
cd fpl-buddy
pio run                                          # build
pio run --target upload                          # flash (see upload notes below)
pio device monitor                               # serial @ 115200
```

### Uploading

The ESP32-C3 on this board has **no auto-reset circuit**, so you must enter
bootloader mode manually every time:

1. Hold **BOOT** (GPIO 9).
2. While holding BOOT, tap **RESET**.
3. Release BOOT.
4. Run `pio run --target upload`.
5. Tap RESET again to boot the new firmware.

**Cable note:** USB-C to USB-A works more reliably than USB-C to USB-C on this
board. Make sure the cable carries data, not just power.

### Serial output

```
=== ESP32-2424S012C LVGL Test ===
Display initialized
Display: 240x240, rotation: 2
UI setup complete
Tap the screen to see touch coordinates
```

While you tap the display:

```
TOUCH down   x: 45  y:182
TOUCH press  x: 46  y:183
TOUCH press  x: 48  y:181
TOUCH up
```

---

## platformio.ini explained

```ini
[platformio]
boards_dir = boards          # Tell PlatformIO where our custom board JSONs live
```

```ini
[env:esp32-2424S012C]
platform  = espressif32      # Espressif IDF-based platform
board     = esp32-2424S012C  # Picked up from boards/ directory
framework = arduino          # Arduino abstraction layer on top of ESP-IDF
```

### lib_deps

```ini
lib_deps =
    https://github.com/rzeldent/esp32-smartdisplay.git
```

The **esp32-smartdisplay** library does everything the board needs in one call
(`smartdisplay_init()`): initialises the GC9A01 SPI panel via ESP-IDF's
`esp_lcd` component, sets up DMA transfers, configures the CST816S touch
controller, registers both as LVGL display/indev drivers, and starts the
backlight PWM. It pulls in **LVGL v9** as its own dependency — no need to
declare LVGL separately.

### build_flags — what each one does

| Flag | Purpose |
|---|---|
| `-Ofast` | Compiler optimisation. Slightly faster than `-O2`; fine for bare-metal. |
| `-Wall` | Enable all compiler warnings — keeps the code clean. |
| `-D BOARD_NAME="${this.board}"` | Injects the env name as a string macro so firmware can identify itself at runtime. |
| `-D CORE_DEBUG_LEVEL=ARDUHAL_LOG_LEVEL_DEBUG` | ESP-IDF serial logger verbosity. `DEBUG` shows the smartdisplay DMA init messages and any transfer warnings that help diagnose tear-line issues. Switch to `INFO` or `WARN` for a quieter console once everything is working. |
| `-D LV_CONF_INCLUDE_SIMPLE` | Tells LVGL to find `lv_conf.h` by scanning the normal include paths (i.e. our `include/` dir) instead of requiring an absolute file path macro. |
| `-D DISPLAY_MIRROR_Y=true` | **Overrides** the `DISPLAY_MIRROR_Y=false` baked into the board JSON. See *Display orientation* below. |

### Display orientation — why MIRROR_Y is overridden

The board JSON sets `DISPLAY_MIRROR_X=true` because that is the raw default for
this particular GC9A01 panel. The smartdisplay library's rotation callback
derives the final mirror flags from those two values:

| LVGL rotation | mirror X applied | mirror Y applied |
|---|---|---|
| 0 | `MIRROR_X` (true) | `MIRROR_Y` |
| 180 | `!MIRROR_X` (false) | `!MIRROR_Y` |

With the board's original `MIRROR_Y=false` and `ROTATION_180` the result was
`mirror(false, true)` — letters the right way round but each glyph flipped
vertically. Setting `MIRROR_Y=true` in our build flags makes `ROTATION_180`
compute `mirror(false, false)`, which is the correct orientation. The override
flag appears after the board flags on the compiler command line so GCC uses it.

---

## Board definition codes (`boards/esp32-2424S012C.json`)

The board JSON is the single source of truth for every hardware constant.  The
smartdisplay library reads these at compile time — nothing is hard-coded in
application code.  Key groups:

### Display bus (SPI)

| Define | Value | Meaning |
|---|---|---|
| `DISPLAY_GC9A01_SPI` | (present) | Selects the GC9A01-over-SPI driver |
| `GC9A01_SPI_HOST` | `SPI2_HOST` | Which ESP32 SPI peripheral to use |
| `GC9A01_SPI_BUS_MOSI` | 7 | MOSI GPIO |
| `GC9A01_SPI_BUS_SCLK` | 6 | Clock GPIO |
| `GC9A01_SPI_CONFIG_CS` | 10 | Chip-select GPIO |
| `GC9A01_SPI_CONFIG_DC` | 2 | Data/command GPIO |
| `GC9A01_SPI_CONFIG_PCLK_HZ` | 80 MHz | SPI clock speed |

### Display panel

| Define | Value | Meaning |
|---|---|---|
| `DISPLAY_WIDTH` / `HEIGHT` | 240 | Panel resolution |
| `DISPLAY_IPS` | (present) | Tells the driver to invert colours (IPS characteristic) |
| `DISPLAY_MIRROR_X` | true | Hardware X-mirror for correct default orientation |
| `DISPLAY_MIRROR_Y` | false | (overridden to `true` in platformio.ini — see above) |
| `DISPLAY_BCKL` | 3 | Backlight PWM GPIO |

### DMA / buffer tuning

| Define | Value | Meaning |
|---|---|---|
| `LVGL_BUFFER_PIXELS` | `WIDTH*HEIGHT/4` (14 400 px) | LVGL draw-buffer size. Quarter-screen = decent balance between RAM and flush count. |
| `SMARTDISPLAY_DMA_BUFFER_SIZE` | 65 536 bytes | Size of the DMA-capable heap buffer. Larger = fewer chunks per flush. |
| `SMARTDISPLAY_DMA_QUEUE_SIZE` | 12 | FreeRTOS queue depth for pending DMA transfers. |
| `SMARTDISPLAY_DMA_CHUNK_THRESHOLD` | 2 048 bytes | Transfers below this skip DMA and go directly via CPU — avoids DMA overhead on tiny updates. |

### Touch (CST816S over I2C)

| Define | Value | Meaning |
|---|---|---|
| `TOUCH_CST816S_I2C` | (present) | Selects the CST816S driver |
| `CST816S_I2C_HOST` | `I2C_NUM_0` | Which I2C peripheral |
| `CST816S_I2C_CONFIG_SDA` / `SCL` | 4 / 5 | I2C GPIOs |
| `CST816S_TOUCH_CONFIG_RST` / `INT` | 1 / 0 | Reset and interrupt GPIOs |
| `CST816S_TOUCH_CONFIG_X/Y_MAX` | `DISPLAY_WIDTH/HEIGHT` | Coordinate range the driver reports |
| `TOUCH_MIRROR_X/Y` | false | Raw touch coordinates match the display — no software flip needed |

---

## lv_conf.h notes

Copied from LVGL's `lv_conf_template.h` and enabled (`#if 1`). Only two values
were changed from the template defaults:

| Setting | Value | Why |
|---|---|---|
| `LV_FONT_MONTSERRAT_20` | 1 | Reserved for later use; currently not referenced so the linker strips it |
| `LV_USE_LOG` | 1 | Routes LVGL warnings/errors through the ESP-IDF logger to serial — useful for DMA tear-line diagnosis |

Everything else (color depth 16, 48 KB memory pool, widget set, theme) is the
LVGL default and works fine on this board.

---

## Troubleshooting

| Symptom | Likely cause | Fix |
|---|---|---|
| Upload times out | Not in bootloader | Hold BOOT, tap RESET, release BOOT, then upload |
| Upload times out | Charge-only cable | Use a data-capable USB-C cable; USB-C→A adapters tend to be more reliable |
| Display stays black | Backlight off / init failed | Check serial for errors; the library sets backlight to 50% on init |
| Display stays black | Submodule missing | `git submodule update --init --recursive` |
| Horizontal tear line (~1/3 down) | DMA transfer timing vs panel scan | Under investigation — check serial for `queue full` or `DMA transfer failed` warnings |
| Text garbled / flipped | Wrong mirror flags | See *Display orientation* section above |
| No touch output on serial | Touch controller not detected | Check I2C wiring; serial should show CST816S init messages |

---

## Resources

- [esp32-smartdisplay](https://github.com/rzeldent/esp32-smartdisplay) — library source + examples
- [platformio-espressif32-sunton](https://github.com/rzeldent/platformio-espressif32-sunton) — board definitions (our `boards/` submodule)
- [LVGL v9 docs](https://docs.lvgl.io/9.4/)
- [GC9A01A datasheet](https://www.buydisplay.com/download/ic/GC9A01A.pdf)
