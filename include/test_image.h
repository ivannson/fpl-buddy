#ifndef TEST_IMAGE_H
#define TEST_IMAGE_H

#include <lvgl.h>

#ifndef LV_ATTRIBUTE_MEM_ALIGN
#define LV_ATTRIBUTE_MEM_ALIGN
#endif

// Simple 32x32 white circle on transparent background
// Using RGB565 format for efficiency
// Each pixel is 2 bytes (RGB565)

const LV_ATTRIBUTE_MEM_ALIGN uint8_t test_circle_map[] = {
    // This is a simplified placeholder - in production you'd use LVGL image converter
    // For now, we'll create the circle programmatically in the code instead
};

// We'll draw the circle using LVGL drawing functions instead of using a bitmap
// This is simpler for testing purposes

#endif // TEST_IMAGE_H
