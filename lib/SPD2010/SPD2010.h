/**
 * SPD2010.h - QSPI display driver for Waveshare ESP32-S3-Touch-LCD-1.46
 * Based on Espressif's esp_lcd_spd2010 component
 */

#ifndef SPD2010_H
#define SPD2010_H

#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <driver/spi_master.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>

/* Display dimensions (override via build flags DISPLAY_WIDTH / DISPLAY_HEIGHT) */
#ifdef DISPLAY_WIDTH
#define SPD2010_WIDTH DISPLAY_WIDTH
#else
#define SPD2010_WIDTH 412
#endif

#ifdef DISPLAY_HEIGHT
#define SPD2010_HEIGHT DISPLAY_HEIGHT
#else
#define SPD2010_HEIGHT 412
#endif

/* QSPI pins */
#define SPD2010_QSPI_CS     21
#define SPD2010_QSPI_SCK    40
#define SPD2010_QSPI_DATA0  46
#define SPD2010_QSPI_DATA1  45
#define SPD2010_QSPI_DATA2  42
#define SPD2010_QSPI_DATA3  41

/* Other pins */
#define SPD2010_BL_PIN      5
#define SPD2010_TE_PIN      18

/* Touch I2C */
#define SPD2010_TOUCH_SDA   11
#define SPD2010_TOUCH_SCL   10
#define SPD2010_TOUCH_INT   4
#define SPD2010_TOUCH_ADDR  0x53

/* I2C Expander for reset */
#define SPD2010_EXPANDER_ADDR   0x20
#define SPD2010_EXIO_LCD_RST    2
#define SPD2010_EXIO_TP_RST     1

class SPD2010Display {
public:
    SPD2010Display();
    ~SPD2010Display();

    bool begin();
    void drawBitmap(int x, int y, int w, int h, const uint8_t *data);
    // Sends big-endian RGB565 pixels straight from `data` (DMA-capable memory), without copying. x and w must be
    // multiples of 4 (the panel's column alignment). Returns once the transfer is queued; the transfer-done
    // callback then runs, from an interrupt, when `data` may be reused. Returns false if nothing was queued.
    bool drawBitmapAsync(int x, int y, int w, int h, const void *data);
    void setTransferDoneCallback(void (*cb)(void *ctx), void *ctx);
    void fillScreen(uint16_t color);
    void setBacklight(uint8_t brightness);

    int getWidth() { return SPD2010_WIDTH; }
    int getHeight() { return SPD2010_HEIGHT; }

private:
    esp_lcd_panel_handle_t _panel;
    esp_lcd_panel_io_handle_t _io;
    bool _initialized;

    void (*_doneCb)(void *ctx) = nullptr;
    void *_doneCtx = nullptr;

    static bool onColorTransDone(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *edata, void *ctx);
    void resetDisplay();
    bool initPanel();
    void expanderWrite(uint8_t pin, uint8_t value);
};

class SPD2010Touch {
public:
    SPD2010Touch();
    bool begin();
    bool getTouch(int *x, int *y);
    bool isTouched();

private:
    bool _initialized;
};

#endif /* SPD2010_H */
