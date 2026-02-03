#include <Arduino.h>
#include <esp32_smartdisplay.h>

// LVGL objects
lv_obj_t *arc_progress;
lv_obj_t *label_status;
lv_obj_t *circle_obj;

// Track last tick for LVGL
auto lv_last_tick = millis();

// Animation callback
void arc_anim_cb(void *var, int32_t value)
{
    lv_arc_set_value((lv_obj_t *)var, value);
}

// Touch event callback - fires on every scan while finger is down
void touch_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_event_get_indev(e);

    if (code == LV_EVENT_PRESSING)
    {
        lv_point_t point;
        lv_indev_get_point(indev, &point);
        Serial.printf("TOUCH press  x:%3d  y:%3d\n", point.x, point.y);
    }
    else if (code == LV_EVENT_PRESSED)
    {
        lv_point_t point;
        lv_indev_get_point(indev, &point);
        Serial.printf("TOUCH down   x:%3d  y:%3d\n", point.x, point.y);
    }
    else if (code == LV_EVENT_RELEASED)
    {
        Serial.println("TOUCH up");
    }
}

void setup_ui()
{
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x000000), LV_PART_MAIN);

    // Screen must be clickable to receive touch events
    lv_obj_add_flag(screen, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event(screen, touch_event_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event(screen, touch_event_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event(screen, touch_event_cb, LV_EVENT_RELEASED, NULL);

    // Animated arc (outer progress ring)
    arc_progress = lv_arc_create(screen);
    lv_obj_set_size(arc_progress, 220, 220);
    lv_obj_center(arc_progress);

    lv_arc_set_rotation(arc_progress, 270);
    lv_arc_set_bg_angles(arc_progress, 0, 360);
    lv_arc_set_value(arc_progress, 0);
    lv_arc_set_range(arc_progress, 0, 100);

    lv_obj_set_style_arc_color(arc_progress, lv_color_hex(0xFFA500), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(arc_progress, 12, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc_progress, lv_color_hex(0x333333), LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc_progress, 12, LV_PART_MAIN);

    lv_obj_remove_style(arc_progress, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(arc_progress, LV_OBJ_FLAG_CLICKABLE);

    // White circle (football icon placeholder)
    circle_obj = lv_obj_create(screen);
    lv_obj_set_size(circle_obj, 64, 64);
    lv_obj_center(circle_obj);
    lv_obj_set_y(circle_obj, -30);

    lv_obj_set_style_radius(circle_obj, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(circle_obj, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_border_width(circle_obj, 0, LV_PART_MAIN);
    lv_obj_remove_flag(circle_obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(circle_obj, LV_OBJ_FLAG_CLICKABLE);

    // "LVGL OK" label
    label_status = lv_label_create(screen);
    lv_label_set_text(label_status, "LVGL OK");
    lv_obj_set_style_text_color(label_status, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_center(label_status);
    lv_obj_set_y(label_status, 30);

    // Arc animation: 0 -> 100 over 5 seconds, loop forever
    lv_anim_t arc_animation;
    lv_anim_init(&arc_animation);
    lv_anim_set_var(&arc_animation, arc_progress);
    lv_anim_set_exec_cb(&arc_animation, arc_anim_cb);
    lv_anim_set_values(&arc_animation, 0, 100);
    lv_anim_set_duration(&arc_animation, 5000);
    lv_anim_set_repeat_count(&arc_animation, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_playback_duration(&arc_animation, 0);
    lv_anim_start(&arc_animation);

    Serial.println("UI setup complete");
}

void setup()
{
    Serial.begin(115200);
    Serial.println("\n\n=== ESP32-2424S012C LVGL Test ===");

    smartdisplay_init();
    Serial.println("Display initialized");

    // Rotate 180 to correct the horizontal mirror baked into the board definition.
    // The library's resolution-changed callback adjusts the hardware mirror flags
    // automatically when rotation is changed.
    auto display = lv_display_get_default();
    lv_display_set_rotation(display, LV_DISPLAY_ROTATION_180);

    Serial.printf("Display: %dx%d, rotation: %d\n",
                  lv_display_get_horizontal_resolution(display),
                  lv_display_get_vertical_resolution(display),
                  lv_display_get_rotation(display));

    setup_ui();

    Serial.println("Tap the screen to see touch coordinates");
}

void loop()
{
    auto const now = millis();
    lv_tick_inc(now - lv_last_tick);
    lv_last_tick = now;

    lv_timer_handler();
}
