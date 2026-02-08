/**
 * SPD2010.cpp - QSPI display driver for Waveshare ESP32-S3-Touch-LCD-1.46
 * Uses Espressif's esp_lcd_spd2010 panel implementation (same path as demo code).
 */

#include "SPD2010.h"

#include <cstring>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include "esp_lcd_spd2010.h"

static const char *TAG = "SPD2010";

SPD2010Display::SPD2010Display() : _panel(nullptr), _io(nullptr), _initialized(false) {
}

SPD2010Display::~SPD2010Display() {
    if (_panel) {
        esp_lcd_panel_del(_panel);
        _panel = nullptr;
    }
}

void SPD2010Display::expanderWrite(uint8_t pin, uint8_t value) {
    static uint8_t output_state = 0xFF;
    if (value) {
        output_state |= (1U << pin);
    } else {
        output_state &= ~(1U << pin);
    }

    Wire.beginTransmission(SPD2010_EXPANDER_ADDR);
    Wire.write(0x01);  // Output register
    Wire.write(output_state);
    Wire.endTransmission();
}

void SPD2010Display::resetDisplay() {
    // Configure all PCA9554A pins as outputs.
    Wire.beginTransmission(SPD2010_EXPANDER_ADDR);
    Wire.write(0x03);  // Configuration register
    Wire.write(0x00);
    Wire.endTransmission();

    // EXIO1: touch reset, EXIO2: LCD reset
    expanderWrite(SPD2010_EXIO_TP_RST, 0);
    expanderWrite(SPD2010_EXIO_LCD_RST, 0);
    delay(50);
    expanderWrite(SPD2010_EXIO_TP_RST, 1);
    expanderWrite(SPD2010_EXIO_LCD_RST, 1);
    delay(50);
}

bool SPD2010Display::initPanel() {
    spi_bus_config_t host_config = {};
    host_config.mosi_io_num = -1;
    host_config.miso_io_num = -1;
    host_config.sclk_io_num = SPD2010_QSPI_SCK;
    host_config.quadwp_io_num = -1;
    host_config.quadhd_io_num = -1;
    host_config.data0_io_num = SPD2010_QSPI_DATA0;
    host_config.data1_io_num = SPD2010_QSPI_DATA1;
    host_config.data2_io_num = SPD2010_QSPI_DATA2;
    host_config.data3_io_num = SPD2010_QSPI_DATA3;
    host_config.data4_io_num = -1;
    host_config.data5_io_num = -1;
    host_config.data6_io_num = -1;
    host_config.data7_io_num = -1;
    host_config.max_transfer_sz = SPD2010_WIDTH * 40 * sizeof(uint16_t);
    host_config.flags = SPICOMMON_BUSFLAG_MASTER;
    host_config.intr_flags = 0;

    esp_err_t ret = spi_bus_initialize(SPI2_HOST, &host_config, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "SPI bus init failed: %d", ret);
        return false;
    }

    esp_lcd_panel_io_spi_config_t io_config = {};
    io_config.cs_gpio_num = SPD2010_QSPI_CS;
    io_config.dc_gpio_num = -1;
    io_config.spi_mode = 3;
    io_config.pclk_hz = 40 * 1000 * 1000;
    io_config.trans_queue_depth = 10;
    io_config.on_color_trans_done = nullptr;
    io_config.user_ctx = nullptr;
    io_config.lcd_cmd_bits = 32;
    io_config.lcd_param_bits = 8;
    io_config.flags.dc_low_on_data = 0;
    io_config.flags.octal_mode = 0;
    io_config.flags.quad_mode = 1;
    io_config.flags.sio_mode = 0;
    io_config.flags.lsb_first = 0;
    io_config.flags.cs_high_active = 0;

    ret = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_config, &_io);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "panel IO init failed: %d", ret);
        return false;
    }

    spd2010_vendor_config_t vendor_config = {};
    vendor_config.init_cmds = nullptr;
    vendor_config.init_cmds_size = 0;
    vendor_config.flags.use_qspi_interface = 1;

    esp_lcd_panel_dev_config_t panel_config = {};
    panel_config.reset_gpio_num = -1;
    panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    panel_config.data_endian = LCD_RGB_DATA_ENDIAN_BIG;
    panel_config.bits_per_pixel = 16;
    panel_config.vendor_config = &vendor_config;

    ret = esp_lcd_new_panel_spd2010(_io, &panel_config, &_panel);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "panel create failed: %d", ret);
        return false;
    }

    ESP_ERROR_CHECK(esp_lcd_panel_reset(_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(_panel, true));

    return true;
}

bool SPD2010Display::begin() {
    ESP_LOGI(TAG, "Initializing SPD2010 display %dx%d", SPD2010_WIDTH, SPD2010_HEIGHT);

    Wire.begin(SPD2010_TOUCH_SDA, SPD2010_TOUCH_SCL, 400000);
    resetDisplay();

    pinMode(SPD2010_TE_PIN, OUTPUT);

    if (!initPanel()) {
        return false;
    }

    pinMode(SPD2010_BL_PIN, OUTPUT);
    digitalWrite(SPD2010_BL_PIN, HIGH);

    fillScreen(0x0000);
    _initialized = true;

    ESP_LOGI(TAG, "SPD2010 display initialized");
    return true;
}

void SPD2010Display::fillScreen(uint16_t color) {
    if (!_panel) {
        return;
    }

    const uint16_t swapped = static_cast<uint16_t>((color >> 8) | (color << 8));
    uint16_t *line = static_cast<uint16_t *>(heap_caps_malloc(SPD2010_WIDTH * sizeof(uint16_t), MALLOC_CAP_DMA));
    if (!line) {
        ESP_LOGE(TAG, "fillScreen buffer allocation failed");
        return;
    }

    for (int i = 0; i < SPD2010_WIDTH; ++i) {
        line[i] = swapped;
    }

    for (int y = 0; y < SPD2010_HEIGHT; ++y) {
        esp_lcd_panel_draw_bitmap(_panel, 0, y, SPD2010_WIDTH, y + 1, line);
    }

    heap_caps_free(line);
}

void SPD2010Display::drawBitmap(int x, int y, int w, int h, const uint8_t *data) {
    if (!_initialized || !_panel || !data || w <= 0 || h <= 0) {
        return;
    }

    if (x < 0 || y < 0 || (x + w) > SPD2010_WIDTH || (y + h) > SPD2010_HEIGHT) {
        return;
    }

    // SPD2010 requires X window aligned to 4 pixels.
    const int x1_aligned = x & ~0x3;
    const int x2_raw = x + w - 1;
    int x2_aligned = x2_raw | 0x3;
    if (x2_aligned >= SPD2010_WIDTH) {
        x2_aligned = SPD2010_WIDTH - 1;
    }

    const int out_w = x2_aligned - x1_aligned + 1;
    const int left_pad = x - x1_aligned;

    // Reusable chunk buffer to avoid large per-flush allocations and fragmentation.
    static uint16_t *draw_buf = nullptr;
    static size_t draw_buf_pixels = 0;

    int chunk_rows = (h < 16) ? h : 16;
    while (chunk_rows > 0) {
        const size_t required_pixels = static_cast<size_t>(out_w) * static_cast<size_t>(chunk_rows);
        if (draw_buf_pixels < required_pixels) {
            uint16_t *new_buf = static_cast<uint16_t *>(
                heap_caps_malloc(required_pixels * sizeof(uint16_t), MALLOC_CAP_DMA));
            if (new_buf) {
                if (draw_buf) {
                    heap_caps_free(draw_buf);
                }
                draw_buf = new_buf;
                draw_buf_pixels = required_pixels;
            }
        }

        if (draw_buf_pixels >= required_pixels && draw_buf) {
            break;
        }
        chunk_rows /= 2;
    }

    if (chunk_rows <= 0 || !draw_buf) {
        ESP_LOGE(TAG, "drawBitmap buffer allocation failed");
        return;
    }

    const uint16_t *src = reinterpret_cast<const uint16_t *>(data);
    for (int row_start = 0; row_start < h; row_start += chunk_rows) {
        const int rows = ((row_start + chunk_rows) <= h) ? chunk_rows : (h - row_start);

        for (int row = 0; row < rows; ++row) {
            for (int col = 0; col < out_w; ++col) {
                int src_col = col - left_pad;
                if (src_col < 0) src_col = 0;
                if (src_col >= w) src_col = w - 1;

                const uint16_t px = src[(row_start + row) * w + src_col];
                draw_buf[row * out_w + col] = static_cast<uint16_t>((px >> 8) | (px << 8));
            }
        }

        esp_lcd_panel_draw_bitmap(
            _panel,
            x1_aligned,
            y + row_start,
            x2_aligned + 1,
            y + row_start + rows,
            draw_buf);
    }
}

void SPD2010Display::setBacklight(uint8_t brightness) {
    pinMode(SPD2010_BL_PIN, OUTPUT);
    digitalWrite(SPD2010_BL_PIN, brightness > 0 ? HIGH : LOW);
}

SPD2010Touch::SPD2010Touch() : _initialized(false) {
}

bool SPD2010Touch::begin() {
    Wire.beginTransmission(SPD2010_TOUCH_ADDR);
    if (Wire.endTransmission() != 0) {
        ESP_LOGE(TAG, "Touch controller not found at 0x%02X", SPD2010_TOUCH_ADDR);
        return false;
    }

    pinMode(SPD2010_TOUCH_INT, INPUT_PULLUP);
    _initialized = true;
    ESP_LOGI(TAG, "SPD2010 touch initialized");
    return true;
}

bool SPD2010Touch::isTouched() {
    return digitalRead(SPD2010_TOUCH_INT) == LOW;
}

static bool touchReadReg16(uint16_t reg, uint8_t *buf, size_t len) {
    Wire.beginTransmission(SPD2010_TOUCH_ADDR);
    Wire.write(static_cast<uint8_t>(reg >> 8));
    Wire.write(static_cast<uint8_t>(reg & 0xFF));
    if (Wire.endTransmission(true) != 0) {
        return false;
    }

    const size_t got = Wire.requestFrom(static_cast<uint8_t>(SPD2010_TOUCH_ADDR), static_cast<uint8_t>(len));
    if (got < len) {
        return false;
    }

    for (size_t i = 0; i < len; ++i) {
        buf[i] = Wire.read();
    }
    return true;
}

static bool touchWriteReg16(uint16_t reg, const uint8_t *buf, size_t len) {
    Wire.beginTransmission(SPD2010_TOUCH_ADDR);
    Wire.write(static_cast<uint8_t>(reg >> 8));
    Wire.write(static_cast<uint8_t>(reg & 0xFF));
    for (size_t i = 0; i < len; ++i) {
        Wire.write(buf[i]);
    }
    return Wire.endTransmission(true) == 0;
}

static void touchWriteCmd(uint16_t reg, uint16_t value) {
    uint8_t data[2] = {
        static_cast<uint8_t>(value & 0xFF),
        static_cast<uint8_t>(value >> 8),
    };
    touchWriteReg16(reg, data, sizeof(data));
}

bool SPD2010Touch::getTouch(int *x, int *y) {
    if (!_initialized || !x || !y) {
        return false;
    }

    static uint32_t last_poll_ms = 0;
    const uint32_t now = millis();
    if (now - last_poll_ms < 8) {
        return false;
    }
    last_poll_ms = now;

    uint8_t status[4] = {};
    if (!touchReadReg16(0x2000, status, sizeof(status))) {
        return false;
    }

    const bool pt_exist = (status[0] & 0x01) != 0;
    const bool gesture = (status[0] & 0x02) != 0;
    const bool aux = (status[0] & 0x08) != 0;
    const bool tic_in_bios = (status[1] & 0x40) != 0;
    const bool tic_in_cpu = (status[1] & 0x20) != 0;
    const bool cpu_run = (status[1] & 0x08) != 0;
    const uint16_t read_len = static_cast<uint16_t>(status[2]) | (static_cast<uint16_t>(status[3]) << 8);

    if (tic_in_bios) {
        touchWriteCmd(0x0200, 0x0001);  // clear int
        touchWriteCmd(0x0400, 0x0001);  // cpu start
        return false;
    }

    if (tic_in_cpu) {
        touchWriteCmd(0x5000, 0x0000);  // point mode
        touchWriteCmd(0x4600, 0x0000);  // start
        touchWriteCmd(0x0200, 0x0001);  // clear int
        return false;
    }

    if (cpu_run && read_len == 0) {
        touchWriteCmd(0x0200, 0x0001);  // clear int
        return false;
    }

    if ((!pt_exist && !gesture) || read_len < 4 || read_len > 64) {
        if (cpu_run && aux) {
            touchWriteCmd(0x0200, 0x0001);  // clear int
        }
        return false;
    }

    uint8_t packet[64] = {};
    if (!touchReadReg16(0x0003, packet, read_len)) {
        return false;
    }

    bool has_point = false;
    if (read_len >= 10) {
        const uint8_t check_id = packet[4];
        if (pt_exist && check_id <= 0x0A) {
            *x = (((packet[7] & 0xF0) << 4) | packet[5]);
            *y = (((packet[7] & 0x0F) << 8) | packet[6]);
            has_point = true;
        }
    }

    // HDP completion handling, same idea as Waveshare reference.
    for (int i = 0; i < 3; ++i) {
        uint8_t hdp_status[8] = {};
        if (!touchReadReg16(0xFC02, hdp_status, sizeof(hdp_status))) {
            break;
        }
        const uint8_t done_status = hdp_status[5];
        const uint16_t next_packet_len = static_cast<uint16_t>(hdp_status[2]) | (static_cast<uint16_t>(hdp_status[3]) << 8);

        if (done_status == 0x82) {
            touchWriteCmd(0x0200, 0x0001);  // clear int
            break;
        }

        if (done_status == 0x00 && next_packet_len > 0 && next_packet_len <= sizeof(packet)) {
            touchReadReg16(0x0003, packet, next_packet_len);
            continue;
        }

        break;
    }

    if (!has_point) {
        return false;
    }

    if (*x >= SPD2010_WIDTH) *x = SPD2010_WIDTH - 1;
    if (*y >= SPD2010_HEIGHT) *y = SPD2010_HEIGHT - 1;

    return true;
}
