#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <SPD2010.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>
#include <lvgl.h>

#include "fpl_config.h"
#include "wifi_config.h"

static SPD2010Display display;
static SPD2010Touch touch;

static constexpr uint16_t kDisplayWidth = SPD2010_WIDTH;
static constexpr uint16_t kDisplayHeight = SPD2010_HEIGHT;
static constexpr size_t kLvglBufPixels = kDisplayWidth * 40;

static lv_display_t *lvglDisp = nullptr;
static uint8_t *lvglBuf = nullptr;

// UI objects
static lv_obj_t *arcProgress = nullptr;
static lv_obj_t *circleObj = nullptr;
static lv_obj_t *labelStatus = nullptr;
static lv_obj_t *labelPoints = nullptr;

static uint32_t lastPollMs = 0;
static uint32_t lastWifiRetryMs = 0;

static void lvglFlushCb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
    const int x1 = area->x1;
    const int y1 = area->y1;
    const int x2 = area->x2;
    const int y2 = area->y2;
    const int w = x2 - x1 + 1;
    const int h = y2 - y1 + 1;

    display.drawBitmap(x1, y1, w, h, px_map);
    lv_display_flush_ready(disp);
}

static uint32_t lvglTickCb(void) {
    return millis();
}

static void lvglTouchCb(lv_indev_t *, lv_indev_data_t *data) {
    int x = 0;
    int y = 0;
    if (touch.getTouch(&x, &y)) {
        data->point.x = x;
        data->point.y = y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

static void arcAnimCb(void *var, int32_t value) {
    lv_arc_set_value(static_cast<lv_obj_t *>(var), static_cast<int16_t>(value));
}

static void updateStatus(const char *text, lv_color_t color = lv_color_hex(0xFFFFFF)) {
    if (!labelStatus) {
        return;
    }
    lv_label_set_text(labelStatus, text);
    lv_obj_set_style_text_color(labelStatus, color, LV_PART_MAIN);
}

static void updatePoints(int points) {
    if (!labelPoints) {
        return;
    }

    char buf[48];
    snprintf(buf, sizeof(buf), "GW Points\n%d", points);
    lv_label_set_text(labelPoints, buf);
}

static void createUi() {
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x000000), LV_PART_MAIN);
    const int min_side = (kDisplayWidth < kDisplayHeight) ? kDisplayWidth : kDisplayHeight;
    const int arc_size = min_side - 8;
    const int ring_w = 20;
    const int center_circle = min_side / 5;

    arcProgress = lv_arc_create(screen);
    lv_obj_set_size(arcProgress, arc_size, arc_size);
    lv_obj_center(arcProgress);

    lv_arc_set_rotation(arcProgress, 270);
    lv_arc_set_bg_angles(arcProgress, 0, 360);
    lv_arc_set_value(arcProgress, 0);
    lv_arc_set_range(arcProgress, 0, 100);

    lv_obj_set_style_arc_color(arcProgress, lv_color_hex(0xFFA500), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(arcProgress, ring_w, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arcProgress, lv_color_hex(0x333333), LV_PART_MAIN);
    lv_obj_set_style_arc_width(arcProgress, ring_w, LV_PART_MAIN);

    lv_obj_remove_style(arcProgress, nullptr, LV_PART_KNOB);
    lv_obj_remove_flag(arcProgress, LV_OBJ_FLAG_CLICKABLE);

    circleObj = lv_obj_create(screen);
    lv_obj_set_size(circleObj, center_circle, center_circle);
    lv_obj_center(circleObj);
    lv_obj_set_y(circleObj, -(center_circle / 2) - 26);
    lv_obj_set_style_radius(circleObj, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(circleObj, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_border_width(circleObj, 0, LV_PART_MAIN);
    lv_obj_remove_flag(circleObj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(circleObj, LV_OBJ_FLAG_CLICKABLE);

    labelPoints = lv_label_create(screen);
    lv_label_set_text(labelPoints, "GW Points\n--");
    lv_obj_set_style_text_color(labelPoints, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_align(labelPoints, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_font(labelPoints, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_align(labelPoints, LV_ALIGN_CENTER, 0, center_circle / 2);

    labelStatus = lv_label_create(screen);
    lv_label_set_text(labelStatus, "Booting...");
    lv_obj_set_style_text_color(labelStatus, lv_color_hex(0xA0A0A0), LV_PART_MAIN);
    lv_obj_set_style_text_align(labelStatus, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(labelStatus, LV_ALIGN_BOTTOM_MID, 0, -18);

    lv_anim_t arcAnimation;
    lv_anim_init(&arcAnimation);
    lv_anim_set_var(&arcAnimation, arcProgress);
    lv_anim_set_exec_cb(&arcAnimation, arcAnimCb);
    lv_anim_set_values(&arcAnimation, 0, 100);
    lv_anim_set_duration(&arcAnimation, 5000);
    lv_anim_set_repeat_count(&arcAnimation, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_playback_duration(&arcAnimation, 0);
    lv_anim_start(&arcAnimation);
}

static bool connectWiFi() {
    if (WiFi.status() == WL_CONNECTED) {
        return true;
    }

    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    const uint32_t startMs = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startMs < 15000) {
        lv_timer_handler();
        delay(100);
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("WiFi connected: %s\n", WiFi.localIP().toString().c_str());
        return true;
    }

    Serial.println("WiFi connect timeout");
    return false;
}

static bool fetchFplGameweekPoints(int &pointsOut) {
    if (WiFi.status() != WL_CONNECTED) {
        return false;
    }

    WiFiClientSecure client;
    client.setInsecure();

    HTTPClient http;
    http.setConnectTimeout(10000);
    http.setTimeout(10000);
    http.setUserAgent("fpl-buddy/1.0");
    http.addHeader("Accept", "application/json");
    http.addHeader("Accept-Encoding", "identity");
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

    String url = "https://fantasy.premierleague.com/api/entry/";
    url += String(FPL_ENTRY_ID);
    url += "/";

    if (!http.begin(client, url)) {
        return false;
    }

    const int code = http.GET();
    if (code != HTTP_CODE_OK) {
        Serial.printf("FPL GET failed, HTTP %d\n", code);
        http.end();
        return false;
    }

    const int contentLen = http.getSize();
    Serial.printf("FPL HTTP 200, Content-Length: %d\n", contentLen);

    const String payload = http.getString();
    Serial.printf("FPL payload bytes: %u\n", static_cast<unsigned>(payload.length()));

    // Use a small fixed document and filter to keep memory use low.
    StaticJsonDocument<64> filter;
    filter["summary_event_points"] = true;

    DynamicJsonDocument doc(1024);
    const auto err = deserializeJson(doc, payload, DeserializationOption::Filter(filter));
    http.end();

    if (err) {
        const size_t previewLen = payload.length() < 200 ? payload.length() : 200;
        Serial.printf("JSON parse error: %s\n", err.c_str());
        Serial.printf("Payload preview: %.*s\n", static_cast<int>(previewLen), payload.c_str());
        return false;
    }

    if (!doc["summary_event_points"].is<int>()) {
        Serial.println("FPL response missing summary_event_points");
        return false;
    }

    pointsOut = doc["summary_event_points"].as<int>();
    return true;
}

void setup() {
    Serial.begin(115200);
    delay(500);

    Serial.println("\\n=== Waveshare ESP32-S3-Touch-LCD-1.46B: FPL Buddy ===");

    if (!display.begin()) {
        Serial.println("Display init failed");
        while (true) {
            delay(1000);
        }
    }

    if (!touch.begin()) {
        Serial.println("Touch init failed (continuing without touch)");
    }

    lv_init();
    lv_tick_set_cb(lvglTickCb);

    lvglBuf = static_cast<uint8_t *>(heap_caps_malloc(kLvglBufPixels * sizeof(lv_color_t), MALLOC_CAP_DMA));
    if (!lvglBuf) {
        Serial.println("LVGL DMA buffer allocation failed");
        while (true) {
            delay(1000);
        }
    }

    lvglDisp = lv_display_create(kDisplayWidth, kDisplayHeight);
    lv_display_set_flush_cb(lvglDisp, lvglFlushCb);
    lv_display_set_buffers(lvglDisp, lvglBuf, nullptr, kLvglBufPixels * sizeof(lv_color_t), LV_DISPLAY_RENDER_MODE_PARTIAL);

    lv_indev_t *touchIndev = lv_indev_create();
    lv_indev_set_type(touchIndev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(touchIndev, lvglTouchCb);

    createUi();

    if (connectWiFi()) {
        updateStatus("WiFi connected", lv_color_hex(0x38D39F));
    } else {
        updateStatus("WiFi not connected", lv_color_hex(0xFF5A5A));
    }

    lastPollMs = 0;
    lastWifiRetryMs = millis();
}

void loop() {
    lv_timer_handler();

    const uint32_t now = millis();

    if (WiFi.status() != WL_CONNECTED) {
        if (now - lastWifiRetryMs >= 10000) {
            lastWifiRetryMs = now;
            updateStatus("Reconnecting WiFi...", lv_color_hex(0xFFCC66));
            if (connectWiFi()) {
                updateStatus("WiFi connected", lv_color_hex(0x38D39F));
            } else {
                updateStatus("WiFi not connected", lv_color_hex(0xFF5A5A));
            }
        }
        delay(5);
        return;
    }

    if (lastPollMs == 0 || now - lastPollMs >= FPL_POLL_INTERVAL_MS) {
        lastPollMs = now;
        updateStatus("Fetching FPL points...", lv_color_hex(0xFFCC66));

        int gwPoints = 0;
        if (fetchFplGameweekPoints(gwPoints)) {
            updatePoints(gwPoints);
            updateStatus("FPL updated", lv_color_hex(0x38D39F));
            Serial.printf("FPL GW points: %d\n", gwPoints);
        } else {
            updateStatus("FPL fetch failed", lv_color_hex(0xFF5A5A));
        }
    }

    delay(5);
}
