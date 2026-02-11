/*
 * Custom board configuration for Waveshare ESP32-S3-Touch-LCD-1.46
 * SPD2010 QSPI display 412x412, SPD2010 touch (I2C)
 */

#pragma once

// *INDENT-OFF*

/**
 * Enable custom board configuration
 */
#define ESP_PANEL_BOARD_DEFAULT_USE_CUSTOM  (1)

#if ESP_PANEL_BOARD_DEFAULT_USE_CUSTOM

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
///////////////////////// General Board Configuration ///////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
#define ESP_PANEL_BOARD_NAME                "Waveshare:ESP32-S3-Touch-LCD-1.46"
#define ESP_PANEL_BOARD_WIDTH               (412)
#define ESP_PANEL_BOARD_HEIGHT              (412)

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////////////// LCD Panel Configuration ////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
#define ESP_PANEL_BOARD_USE_LCD             (1)

#if ESP_PANEL_BOARD_USE_LCD
/**
 * LCD controller: SPD2010 (Solomon Systech)
 */
#define ESP_PANEL_BOARD_LCD_CONTROLLER      SPD2010

/**
 * LCD bus type: QSPI
 */
#define ESP_PANEL_BOARD_LCD_BUS_TYPE        (ESP_PANEL_BUS_TYPE_QSPI)

#if ESP_PANEL_BOARD_LCD_BUS_TYPE == ESP_PANEL_BUS_TYPE_QSPI
    #define ESP_PANEL_BOARD_LCD_BUS_SKIP_INIT_HOST      (0)

    /* QSPI bus configuration */
    #define ESP_PANEL_BOARD_LCD_QSPI_HOST_ID        (1)     /* SPI2_HOST */

#if !ESP_PANEL_BOARD_LCD_BUS_SKIP_INIT_HOST
    /* QSPI pins from Waveshare wiki */
    #define ESP_PANEL_BOARD_LCD_QSPI_IO_SCK         (40)
    #define ESP_PANEL_BOARD_LCD_QSPI_IO_DATA0       (46)
    #define ESP_PANEL_BOARD_LCD_QSPI_IO_DATA1       (45)
    #define ESP_PANEL_BOARD_LCD_QSPI_IO_DATA2       (42)
    #define ESP_PANEL_BOARD_LCD_QSPI_IO_DATA3       (41)
#endif

    /* Panel configuration */
    #define ESP_PANEL_BOARD_LCD_QSPI_IO_CS          (21)
    #define ESP_PANEL_BOARD_LCD_QSPI_MODE           (0)
    #define ESP_PANEL_BOARD_LCD_QSPI_CLK_HZ         (40 * 1000 * 1000)  /* 40MHz */
    #define ESP_PANEL_BOARD_LCD_QSPI_CMD_BITS       (32)
    #define ESP_PANEL_BOARD_LCD_QSPI_PARAM_BITS     (8)

#endif /* ESP_PANEL_BOARD_LCD_BUS_TYPE == ESP_PANEL_BUS_TYPE_QSPI */

/**
 * LCD color configuration
 */
#define ESP_PANEL_BOARD_LCD_COLOR_BITS          (ESP_PANEL_LCD_COLOR_BITS_RGB565)
#define ESP_PANEL_BOARD_LCD_COLOR_BGR_ORDER     (0)     /* RGB order */
#define ESP_PANEL_BOARD_LCD_COLOR_INEVRT_BIT    (0)

/**
 * LCD transformation configuration
 */
#define ESP_PANEL_BOARD_LCD_SWAP_XY             (0)
#define ESP_PANEL_BOARD_LCD_MIRROR_X            (0)
#define ESP_PANEL_BOARD_LCD_MIRROR_Y            (0)
#define ESP_PANEL_BOARD_LCD_GAP_X               (0)
#define ESP_PANEL_BOARD_LCD_GAP_Y               (0)

/**
 * LCD reset pin - controlled via IO expander (EXIO2)
 */
#define ESP_PANEL_BOARD_LCD_RST_IO              (-1)    /* Using IO expander, set to -1 */
#define ESP_PANEL_BOARD_LCD_RST_LEVEL           (0)

#endif /* ESP_PANEL_BOARD_USE_LCD */

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////////////// Touch Panel Configuration //////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
#define ESP_PANEL_BOARD_USE_TOUCH               (1)

#if ESP_PANEL_BOARD_USE_TOUCH
/**
 * Touch controller: SPD2010
 */
#define ESP_PANEL_BOARD_TOUCH_CONTROLLER        SPD2010

/**
 * Touch bus type: I2C
 */
#define ESP_PANEL_BOARD_TOUCH_BUS_TYPE          (ESP_PANEL_BUS_TYPE_I2C)

#if ESP_PANEL_BOARD_TOUCH_BUS_TYPE == ESP_PANEL_BUS_TYPE_I2C
    #define ESP_PANEL_BOARD_TOUCH_BUS_SKIP_INIT_HOST        (0)

    /* I2C bus configuration */
    #define ESP_PANEL_BOARD_TOUCH_I2C_HOST_ID           (0)

#if !ESP_PANEL_BOARD_TOUCH_BUS_SKIP_INIT_HOST
    #define ESP_PANEL_BOARD_TOUCH_I2C_CLK_HZ            (400 * 1000)
    #define ESP_PANEL_BOARD_TOUCH_I2C_SCL_PULLUP        (1)
    #define ESP_PANEL_BOARD_TOUCH_I2C_SDA_PULLUP        (1)
    #define ESP_PANEL_BOARD_TOUCH_I2C_IO_SCL            (10)
    #define ESP_PANEL_BOARD_TOUCH_I2C_IO_SDA            (11)
#endif

    /* Touch I2C address: 0x53 for SPD2010 */
    #define ESP_PANEL_BOARD_TOUCH_I2C_ADDRESS           (0x53)

#endif /* ESP_PANEL_BOARD_TOUCH_BUS_TYPE == ESP_PANEL_BUS_TYPE_I2C */

/**
 * Touch transformation
 */
#define ESP_PANEL_BOARD_TOUCH_SWAP_XY           (0)
#define ESP_PANEL_BOARD_TOUCH_MIRROR_X          (0)
#define ESP_PANEL_BOARD_TOUCH_MIRROR_Y          (0)

/**
 * Touch control pins - reset via IO expander (EXIO1)
 */
#define ESP_PANEL_BOARD_TOUCH_RST_IO            (-1)    /* Using IO expander */
#define ESP_PANEL_BOARD_TOUCH_RST_LEVEL         (0)
#define ESP_PANEL_BOARD_TOUCH_INT_IO            (4)
#define ESP_PANEL_BOARD_TOUCH_INT_LEVEL         (0)

#endif /* ESP_PANEL_BOARD_USE_TOUCH */

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
///////////////////////////// Backlight Configuration ///////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
#define ESP_PANEL_BOARD_USE_BACKLIGHT           (1)

#if ESP_PANEL_BOARD_USE_BACKLIGHT
/**
 * Backlight control via GPIO (simple on/off)
 */
#define ESP_PANEL_BOARD_BACKLIGHT_TYPE          (ESP_PANEL_BACKLIGHT_TYPE_SWITCH_GPIO)

#define ESP_PANEL_BOARD_BACKLIGHT_IO            (5)
#define ESP_PANEL_BOARD_BACKLIGHT_ON_LEVEL      (1)     /* High level = on */

#define ESP_PANEL_BOARD_BACKLIGHT_IDLE_OFF      (0)

#endif /* ESP_PANEL_BOARD_USE_BACKLIGHT */

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
///////////////////////////// IO Expander Configuration /////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
#define ESP_PANEL_BOARD_USE_EXPANDER            (1)

#if ESP_PANEL_BOARD_USE_EXPANDER
/**
 * IO Expander: PCA9554A (TCA95XX compatible)
 */
#define ESP_PANEL_BOARD_EXPANDER_CHIP           TCA95XX_8BIT

#define ESP_PANEL_BOARD_EXPANDER_SKIP_INIT_HOST     (1)     /* Share I2C with touch */
#define ESP_PANEL_BOARD_EXPANDER_I2C_HOST_ID        (0)

#if !ESP_PANEL_BOARD_EXPANDER_SKIP_INIT_HOST
#define ESP_PANEL_BOARD_EXPANDER_I2C_CLK_HZ         (400 * 1000)
#define ESP_PANEL_BOARD_EXPANDER_I2C_SCL_PULLUP     (1)
#define ESP_PANEL_BOARD_EXPANDER_I2C_SDA_PULLUP     (1)
#define ESP_PANEL_BOARD_EXPANDER_I2C_IO_SCL         (10)
#define ESP_PANEL_BOARD_EXPANDER_I2C_IO_SDA         (11)
#endif

/* PCA9554A address */
#define ESP_PANEL_BOARD_EXPANDER_I2C_ADDRESS        (0x20)

#endif /* ESP_PANEL_BOARD_USE_EXPANDER */

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/////////////////////// Pre/Post begin functions for reset control //////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/**
 * Post-begin function for IO expander - set up reset pins
 */
#define ESP_PANEL_BOARD_EXPANDER_POST_BEGIN_FUNCTION(p) \
    {  \
        auto board = static_cast<Board *>(p);  \
        auto expander = board->getExpander();  \
        if (expander) { \
            /* EXIO1 = Touch RST, EXIO2 = LCD RST */ \
            /* Set as outputs and pull high (release reset) */ \
            expander->pinMode(1, OUTPUT); \
            expander->pinMode(2, OUTPUT); \
            expander->digitalWrite(1, LOW);  /* Reset touch */ \
            expander->digitalWrite(2, LOW);  /* Reset LCD */ \
            delay(100); \
            expander->digitalWrite(1, HIGH); /* Release touch */ \
            expander->digitalWrite(2, HIGH); /* Release LCD */ \
            delay(100); \
        } \
        return true;    \
    }

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////// File Version ////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
#define ESP_PANEL_BOARD_CUSTOM_FILE_VERSION_MAJOR 1
#define ESP_PANEL_BOARD_CUSTOM_FILE_VERSION_MINOR 2
#define ESP_PANEL_BOARD_CUSTOM_FILE_VERSION_PATCH 0

#endif /* ESP_PANEL_BOARD_DEFAULT_USE_CUSTOM */

// *INDENT-ON*
