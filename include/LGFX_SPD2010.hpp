/**
 * LovyanGFX configuration for Waveshare ESP32-S3-Touch-LCD-1.46
 * SPD2010 QSPI display 412x412
 */

#pragma once

#define LGFX_USE_V1
#include <LovyanGFX.hpp>

class LGFX_SPD2010 : public lgfx::LGFX_Device
{
    /* Panel class for SPD2010 QSPI display */
    lgfx::Panel_Device _panel_instance;
    lgfx::Bus_QSPI     _bus_instance;
    lgfx::Light_PWM    _light_instance;
    lgfx::Touch_I2C    _touch_instance;

public:
    LGFX_SPD2010(void)
    {
        /* Bus configuration */
        {
            auto cfg = _bus_instance.config();
            cfg.freq_write = 40000000;      /* SPI clock for write (40MHz) */
            cfg.freq_read  = 16000000;      /* SPI clock for read */
            cfg.spi_host   = SPI2_HOST;
            cfg.spi_mode   = 0;
            cfg.dma_channel = SPI_DMA_CH_AUTO;
            cfg.use_lock   = true;
            cfg.pin_sclk   = 40;            /* SCK */
            cfg.pin_io0    = 46;            /* DATA0 */
            cfg.pin_io1    = 45;            /* DATA1 */
            cfg.pin_io2    = 42;            /* DATA2 */
            cfg.pin_io3    = 41;            /* DATA3 */

            _bus_instance.config(cfg);
            _panel_instance.setBus(&_bus_instance);
        }

        /* Panel configuration */
        {
            auto cfg = _panel_instance.config();
            cfg.pin_cs   = 21;              /* CS pin */
            cfg.pin_rst  = -1;              /* RST via IO expander */
            cfg.pin_busy = -1;

            cfg.panel_width  = 412;
            cfg.panel_height = 412;
            cfg.offset_x     = 0;
            cfg.offset_y     = 0;
            cfg.offset_rotation = 0;
            cfg.readable = false;
            cfg.invert   = false;
            cfg.rgb_order = false;
            cfg.dlen_16bit = false;
            cfg.bus_shared = false;

            _panel_instance.config(cfg);
        }

        /* Backlight configuration */
        {
            auto cfg = _light_instance.config();
            cfg.pin_bl = 5;
            cfg.invert = false;
            cfg.freq   = 44100;
            cfg.pwm_channel = 7;

            _light_instance.config(cfg);
            _panel_instance.setLight(&_light_instance);
        }

        /* Touch configuration */
        {
            auto cfg = _touch_instance.config();
            cfg.pin_sda  = 11;
            cfg.pin_scl  = 10;
            cfg.pin_int  = 4;
            cfg.pin_rst  = -1;              /* RST via IO expander */
            cfg.i2c_addr = 0x53;
            cfg.i2c_port = 0;
            cfg.freq     = 400000;
            cfg.x_min    = 0;
            cfg.x_max    = 411;
            cfg.y_min    = 0;
            cfg.y_max    = 411;

            _touch_instance.config(cfg);
            _panel_instance.setTouch(&_touch_instance);
        }

        setPanel(&_panel_instance);
    }

    /* Initialize IO expander and perform reset */
    bool initExpander()
    {
        Wire.begin(11, 10, 400000);

        /* Configure PCA9554A as output */
        Wire.beginTransmission(0x20);
        Wire.write(0x03);   /* Config register */
        Wire.write(0x00);   /* All outputs */
        if (Wire.endTransmission() != 0) {
            return false;   /* IO expander not found */
        }

        /* Reset sequence */
        setExpanderPin(1, LOW);   /* Touch RST */
        setExpanderPin(2, LOW);   /* LCD RST */
        delay(100);
        setExpanderPin(1, HIGH);
        setExpanderPin(2, HIGH);
        delay(100);

        return true;
    }

    void setExpanderPin(uint8_t pin, uint8_t value)
    {
        static uint8_t output_state = 0xFF;
        if (value) {
            output_state |= (1 << pin);
        } else {
            output_state &= ~(1 << pin);
        }

        Wire.beginTransmission(0x20);
        Wire.write(0x01);   /* Output register */
        Wire.write(output_state);
        Wire.endTransmission();
    }
};
