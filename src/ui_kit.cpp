#include "ui_kit.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

uint32_t uiToneColor(UiTone tone) {
    switch (tone) {
        case UiTone::Accent: return kUiAccent;
        case UiTone::Positive: return kUiPositive;
        case UiTone::Negative: return kUiNegative;
        case UiTone::Warning: return kUiWarning;
        case UiTone::Neutral: break;
    }
    return kUiTextSecondary;
}

int uiSafeWidth(int cy, int height, int margin) {
    const int r = kUiScreenSize / 2;
    const int edge = (cy < 0 ? -cy : cy) + height / 2;
    if (edge >= r) {
        return 0;
    }
    const int chord = static_cast<int>(2.0f * sqrtf(static_cast<float>(r * r - edge * edge)));
    const int w = chord - 2 * margin;
    return w > 0 ? w : 0;
}

void uiFormatThousands(int32_t value, char *out, size_t outLen) {
    if (!out || outLen == 0) {
        return;
    }
    char raw[16];
    snprintf(raw, sizeof(raw), "%ld", static_cast<long>(value < 0 ? -static_cast<int64_t>(value) : value));
    const size_t rawLen = strlen(raw);
    size_t o = 0;
    if (value < 0 && o + 1 < outLen) {
        out[o++] = '-';
    }
    for (size_t i = 0; i < rawLen && o + 1 < outLen; ++i) {
        if (i > 0 && (rawLen - i) % 3 == 0) {
            out[o++] = ',';
            if (o + 1 >= outLen) {
                break;
            }
        }
        out[o++] = raw[i];
    }
    out[o] = '\0';
}

// --- Components ---------------------------------------------------------------------------------------------

lv_obj_t *uiLabel(lv_obj_t *parent, const lv_font_t *font, uint32_t color, lv_text_align_t align) {
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_text_align(label, align, LV_PART_MAIN);
    return label;
}

void uiSingleLine(lv_obj_t *label) {
    const lv_font_t *font = lv_obj_get_style_text_font(label, LV_PART_MAIN);
    lv_obj_set_height(label, lv_font_get_line_height(font));
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
}

void uiSetText(lv_obj_t *label, const char *text) {
    if (!label || !text) {
        return;
    }
    const char *current = lv_label_get_text(label);
    if (current && strcmp(current, text) == 0) {
        return;
    }
    lv_label_set_text(label, text);
}

void uiSetTextColor(lv_obj_t *label, uint32_t color) {
    if (!label) {
        return;
    }
    const lv_color_t target = lv_color_hex(color);
    if (!lv_color_eq(lv_obj_get_style_text_color(label, LV_PART_MAIN), target)) {
        lv_obj_set_style_text_color(label, target, LV_PART_MAIN);
    }
}

lv_obj_t *uiBox(lv_obj_t *parent, int w, int h) {
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, w, h);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    return box;
}

void uiScreenHeader(lv_obj_t *parent, const char *title, const char *subtitle) {
    lv_obj_t *titleLabel = uiLabel(parent, UI_FONT_HEADING, kUiAccent);
    lv_obj_set_style_text_letter_space(titleLabel, 2, LV_PART_MAIN);
    lv_label_set_text(titleLabel, title);
    lv_obj_align(titleLabel, LV_ALIGN_TOP_MID, 0, kUiHeaderY);
    if (subtitle && subtitle[0]) {
        lv_obj_t *subtitleLabel = uiLabel(parent, UI_FONT_META, kUiTextSecondary);
        lv_label_set_text(subtitleLabel, subtitle);
        lv_obj_align(subtitleLabel, LV_ALIGN_TOP_MID, 0, kUiHeaderY + 26);
    }
}

static void stylePill(lv_obj_t *pill, lv_obj_t *label, UiTone tone) {
    const uint32_t color = uiToneColor(tone);
    if (tone == UiTone::Neutral) {
        lv_obj_set_style_bg_color(pill, lv_color_hex(kUiSurfaceRaised), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, LV_PART_MAIN);
    } else {
        lv_obj_set_style_bg_color(pill, lv_color_hex(color), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(pill, LV_OPA_20, LV_PART_MAIN);
    }
    lv_obj_set_style_text_color(label, lv_color_hex(color), LV_PART_MAIN);
}

lv_obj_t *uiStatusPill(lv_obj_t *parent, const char *text, UiTone tone, const lv_font_t *font) {
    lv_obj_t *pill = lv_obj_create(parent);
    lv_obj_remove_style_all(pill);
    lv_obj_set_size(pill, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(pill, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(pill, 11, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(pill, 5, LV_PART_MAIN);
    lv_obj_remove_flag(pill, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(pill, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *label = uiLabel(pill, font, kUiTextSecondary);
    lv_obj_set_style_text_letter_space(label, font == UI_FONT_PILL ? 1 : 0, LV_PART_MAIN);
    lv_label_set_text(label, text);
    stylePill(pill, label, tone);
    return pill;
}

void uiStatusPillSet(lv_obj_t *pill, const char *text, UiTone tone) {
    lv_obj_t *label = pill ? lv_obj_get_child(pill, 0) : nullptr;
    if (!label) {
        return;
    }
    uiSetText(label, text);
    // The pill's user data remembers its tone (+1, so 0 = never set): restyling invalidates the object, and this
    // is called every UI loop.
    void *toneTag = reinterpret_cast<void *>(static_cast<intptr_t>(static_cast<int>(tone) + 1));
    if (lv_obj_get_user_data(pill) != toneTag) {
        stylePill(pill, label, tone);
        lv_obj_set_user_data(pill, toneTag);
    }
}

lv_obj_t *uiListRow(lv_obj_t *parent, int cy, int w, int h) {
    lv_obj_t *row = uiBox(parent, w, h);
    lv_obj_set_style_bg_color(row, lv_color_hex(kUiSurface), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(row, 18, LV_PART_MAIN);
    lv_obj_align(row, LV_ALIGN_CENTER, 0, cy);
    return row;
}

int uiRowCenter(size_t index, size_t count, int h, int gap, int blockCy) {
    const int n = static_cast<int>(count);
    const int blockH = n * h + (n - 1) * gap;
    return blockCy - blockH / 2 + h / 2 + static_cast<int>(index) * (h + gap);
}

void uiEmptyState(lv_obj_t *parent, const char *icon, const char *title, const char *body, UiTone tone) {
    const uint32_t color = uiToneColor(tone);
    lv_obj_t *ring = uiBox(parent, 76, 76);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(ring, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ring, LV_OPA_10, LV_PART_MAIN);
    lv_obj_set_style_border_color(ring, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_border_width(ring, 3, LV_PART_MAIN);
    lv_obj_align(ring, LV_ALIGN_CENTER, 0, -34);
    lv_obj_t *iconLabel = uiLabel(ring, UI_FONT_VALUE, color);
    lv_label_set_text(iconLabel, icon);
    lv_obj_center(iconLabel);

    lv_obj_t *titleLabel = uiLabel(parent, UI_FONT_NAME, kUiTextPrimary);
    lv_obj_set_width(titleLabel, uiSafeWidth(32, 30));
    uiSingleLine(titleLabel);
    lv_label_set_text(titleLabel, title);
    lv_obj_align(titleLabel, LV_ALIGN_CENTER, 0, 32);

    if (body && body[0]) {
        lv_obj_t *bodyLabel = uiLabel(parent, UI_FONT_META, kUiTextSecondary);
        lv_obj_set_width(bodyLabel, uiSafeWidth(76, 44, 28));
        lv_label_set_text(bodyLabel, body);
        lv_obj_align(bodyLabel, LV_ALIGN_TOP_MID, 0, kUiScreenSize / 2 + 54);
    }
}

void uiPageDots(lv_obj_t *parent, uint8_t count, uint8_t active) {
    if (count <= 1) {
        return;
    }
    static constexpr int kDot = 6;
    static constexpr int kActiveW = 18;
    static constexpr int kGap = 6;
    const int totalW = (count - 1) * (kDot + kGap) + kActiveW;
    lv_obj_t *row = uiBox(parent, totalW, kDot);
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, kUiDotsY - kDot / 2);
    int x = 0;
    for (uint8_t i = 0; i < count; ++i) {
        const bool isActive = i == active;
        lv_obj_t *dot = uiBox(row, isActive ? kActiveW : kDot, kDot);
        lv_obj_set_pos(dot, x, 0);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_bg_color(dot, lv_color_hex(isActive ? kUiAccent : kUiSurfaceRaised), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, LV_PART_MAIN);
        x += (isActive ? kActiveW : kDot) + kGap;
    }
}

lv_obj_t *uiDot(lv_obj_t *parent, int size, uint32_t color) {
    lv_obj_t *dot = uiBox(parent, size, size);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(dot, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, LV_PART_MAIN);
    return dot;
}

// --- Battery ------------------------------------------------------------------------------------------------

static constexpr int kBatteryCy = 392;      // centre, below the page dots
static constexpr int kBatteryBodyW = 24;
static constexpr int kBatteryBodyH = 12;
static constexpr int kBatteryBorder = 2;
static constexpr int kBatteryInset = 1;     // gap between the border and the fill
static constexpr int kBatteryNubW = 2;
static constexpr int kBatteryNubH = 5;
static constexpr int kBatteryBlinkMs = 500;

enum class BatteryLevel : uint8_t { Normal, Low, Critical, Flashing };

static lv_obj_t *batteryIcon = nullptr;
static lv_obj_t *batteryBody = nullptr;
static lv_obj_t *batteryNub = nullptr;
static lv_obj_t *batteryFill = nullptr;
static lv_timer_t *batteryBlink = nullptr;
static BatteryLevel batteryLevel = BatteryLevel::Normal;
static int batteryFillW = -1;

// Level for `percent`, with a few percent of hysteresis on the way back up so a reading hovering at a
// threshold does not flicker between colours.
static BatteryLevel batteryLevelFor(int percent, BatteryLevel current) {
    static constexpr int kHysteresis = 3;
    const int flashAt = 10 + (current >= BatteryLevel::Flashing ? kHysteresis : 0);
    const int criticalAt = 20 + (current >= BatteryLevel::Critical ? kHysteresis : 0);
    const int lowAt = 30 + (current >= BatteryLevel::Low ? kHysteresis : 0);
    if (percent <= flashAt) {
        return BatteryLevel::Flashing;
    }
    if (percent <= criticalAt) {
        return BatteryLevel::Critical;
    }
    return percent <= lowAt ? BatteryLevel::Low : BatteryLevel::Normal;
}

static void batteryBlinkCb(lv_timer_t *) {
    const bool visible = lv_obj_get_style_opa(batteryIcon, LV_PART_MAIN) == LV_OPA_COVER;
    lv_obj_set_style_opa(batteryIcon, visible ? LV_OPA_TRANSP : LV_OPA_COVER, LV_PART_MAIN);
}

static void batteryApplyLevel(BatteryLevel level) {
    // Normal: a quiet outline with a light fill. Alerts tint the whole icon so the colour reads at this size.
    uint32_t outline = kUiTextSecondary;
    uint32_t fill = kUiTextPrimary;
    lv_opa_t outlineOpa = LV_OPA_60;
    if (level != BatteryLevel::Normal) {
        outline = fill = level == BatteryLevel::Low ? kUiWarning : kUiNegative;
        outlineOpa = LV_OPA_COVER;
    }
    lv_obj_set_style_border_color(batteryBody, lv_color_hex(outline), LV_PART_MAIN);
    lv_obj_set_style_border_opa(batteryBody, outlineOpa, LV_PART_MAIN);
    lv_obj_set_style_bg_color(batteryNub, lv_color_hex(outline), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(batteryNub, outlineOpa, LV_PART_MAIN);
    lv_obj_set_style_bg_color(batteryFill, lv_color_hex(fill), LV_PART_MAIN);

    if (level == BatteryLevel::Flashing && !batteryBlink) {
        batteryBlink = lv_timer_create(batteryBlinkCb, kBatteryBlinkMs, nullptr);
    } else if (level != BatteryLevel::Flashing && batteryBlink) {
        lv_timer_delete(batteryBlink);
        batteryBlink = nullptr;
        lv_obj_set_style_opa(batteryIcon, LV_OPA_COVER, LV_PART_MAIN);
    }
}

void uiBatteryCreate() {
    if (batteryIcon) {
        return;
    }
    batteryIcon = uiBox(lv_layer_top(), kBatteryBodyW + kBatteryNubW, kBatteryBodyH);
    lv_obj_align(batteryIcon, LV_ALIGN_TOP_MID, 0, kBatteryCy - kBatteryBodyH / 2);

    batteryBody = uiBox(batteryIcon, kBatteryBodyW, kBatteryBodyH);
    lv_obj_set_style_radius(batteryBody, 3, LV_PART_MAIN);
    lv_obj_set_style_border_width(batteryBody, kBatteryBorder, LV_PART_MAIN);
    lv_obj_set_style_pad_all(batteryBody, kBatteryInset, LV_PART_MAIN);

    batteryNub = uiBox(batteryIcon, kBatteryNubW, kBatteryNubH);
    lv_obj_align(batteryNub, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_radius(batteryNub, 1, LV_PART_MAIN);

    batteryFill = uiBox(batteryBody, 0, kBatteryBodyH - 2 * (kBatteryBorder + kBatteryInset));
    lv_obj_align(batteryFill, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_radius(batteryFill, 1, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(batteryFill, LV_OPA_COVER, LV_PART_MAIN);

    batteryApplyLevel(batteryLevel);
    lv_obj_add_flag(batteryIcon, LV_OBJ_FLAG_HIDDEN);
}

void uiBatterySet(int percent) {
    if (!batteryIcon) {
        return;
    }
    percent = percent < 0 ? 0 : (percent > 100 ? 100 : percent);
    lv_obj_remove_flag(batteryIcon, LV_OBJ_FLAG_HIDDEN);

    const BatteryLevel level = batteryLevelFor(percent, batteryLevel);
    if (level != batteryLevel) {
        batteryLevel = level;
        batteryApplyLevel(level);
    }
    // Never quite empty, so the red sliver stays visible at the bottom of the range.
    static constexpr int kMaxFillW = kBatteryBodyW - 2 * (kBatteryBorder + kBatteryInset);
    int w = (kMaxFillW * percent + 50) / 100;
    w = w < 2 ? 2 : w;
    if (w != batteryFillW) {
        batteryFillW = w;
        lv_obj_set_width(batteryFill, w);
    }
}

// --- Motion -------------------------------------------------------------------------------------------------

// The value a number label currently shows lives in its user data; kNoNumber = showing a placeholder.
static constexpr int32_t kNoNumber = INT32_MIN;

static int32_t shownNumber(lv_obj_t *label) {
    return static_cast<int32_t>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(label)));
}

static void numberExec(void *var, int32_t value) {
    lv_obj_t *label = static_cast<lv_obj_t *>(var);
    lv_obj_set_user_data(label, reinterpret_cast<void *>(static_cast<intptr_t>(value)));
    char buf[16];
    uiFormatThousands(value, buf, sizeof(buf));
    uiSetText(label, buf);
}

void uiNumberInit(lv_obj_t *label) {
    lv_obj_set_user_data(label, reinterpret_cast<void *>(static_cast<intptr_t>(kNoNumber)));
}

void uiNumberSet(lv_obj_t *label, int32_t value, bool animate) {
    if (!label) {
        return;
    }
    lv_anim_t *running = lv_anim_get(label, numberExec);
    if (running) {
        if (running->end_value == value) {
            return;
        }
        lv_anim_delete(label, numberExec);
    }
    const int32_t from = shownNumber(label);
    if (from == value) {
        return;
    }
    if (!animate || from == kNoNumber) {
        numberExec(label, value);
        return;
    }
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, label);
    lv_anim_set_exec_cb(&a, numberExec);
    lv_anim_set_values(&a, from, value);
    lv_anim_set_duration(&a, kUiNumberMs);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

void uiNumberClear(lv_obj_t *label, const char *placeholder) {
    if (!label) {
        return;
    }
    lv_anim_delete(label, numberExec);
    uiNumberInit(label);
    uiSetText(label, placeholder);
}

static void opaExec(void *var, int32_t value) {
    lv_obj_set_style_opa(static_cast<lv_obj_t *>(var), static_cast<lv_opa_t>(value), LV_PART_MAIN);
}

void uiFadeIn(lv_obj_t *obj, uint32_t durationMs, uint32_t delayMs) {
    if (!obj) {
        return;
    }
    lv_obj_set_style_opa(obj, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_exec_cb(&a, opaExec);
    lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_duration(&a, durationMs);
    lv_anim_set_delay(&a, delayMs);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

static void widthExec(void *var, int32_t value) {
    lv_obj_set_width(static_cast<lv_obj_t *>(var), value);
}

void uiRevealLeftToRight(lv_obj_t *parent, int x, int y, int w, int h, uint32_t durationMs, uint32_t delayMs) {
    lv_obj_t *frame = uiBox(parent, w, h);
    lv_obj_set_pos(frame, x, y);
    lv_obj_t *cover = uiBox(frame, w, h);
    lv_obj_set_style_bg_color(cover, lv_color_hex(kUiBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(cover, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_align(cover, LV_ALIGN_TOP_RIGHT, 0, 0);  // shrinking the width uncovers the left side first

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, cover);
    lv_anim_set_exec_cb(&a, widthExec);
    lv_anim_set_values(&a, w, 0);
    lv_anim_set_duration(&a, durationMs);
    lv_anim_set_delay(&a, delayMs);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);
}

// The pulse is two concentric rings on the top layer (so it survives screen changes): a crisp outer edge and a
// wider, fainter band inside it that reads as a glow. Only border opacity is animated, which needs no layer.
static lv_obj_t *pulseRing = nullptr;
static constexpr int32_t kPulseSteps = 1000;
static constexpr int32_t kPulsePeakStep = 250;

static void pulseExec(void *var, int32_t step) {
    lv_obj_t *container = static_cast<lv_obj_t *>(var);
    const int32_t level = step < kPulsePeakStep ? (step * 255) / kPulsePeakStep
                                                : ((kPulseSteps - step) * 255) / (kPulseSteps - kPulsePeakStep);
    lv_obj_t *outer = lv_obj_get_child(container, 0);
    lv_obj_t *inner = lv_obj_get_child(container, 1);
    if (outer) {
        lv_obj_set_style_border_opa(outer, static_cast<lv_opa_t>(level), LV_PART_MAIN);
    }
    if (inner) {
        lv_obj_set_style_border_opa(inner, static_cast<lv_opa_t>(level * 35 / 100), LV_PART_MAIN);
    }
}

static void pulseCompleted(lv_anim_t *a) {
    lv_obj_t *container = static_cast<lv_obj_t *>(a->var);
    if (container == pulseRing) {
        pulseRing = nullptr;
    }
    lv_obj_delete_async(container);
}

void uiPulseRing(uint32_t color) {
    if (pulseRing) {
        return;  // one at a time
    }
    lv_obj_t *container = uiBox(lv_layer_top(), kUiScreenSize, kUiScreenSize);
    lv_obj_center(container);
    lv_obj_t *outer = uiBox(container, kUiScreenSize, kUiScreenSize);
    lv_obj_center(outer);
    lv_obj_t *inner = uiBox(container, kUiScreenSize - 6, kUiScreenSize - 6);
    lv_obj_center(inner);
    lv_obj_t *rings[2] = {outer, inner};
    for (lv_obj_t *ring : rings) {
        lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_border_color(ring, lv_color_hex(color), LV_PART_MAIN);
        lv_obj_set_style_border_opa(ring, LV_OPA_TRANSP, LV_PART_MAIN);
    }
    lv_obj_set_style_border_width(outer, 3, LV_PART_MAIN);
    lv_obj_set_style_border_width(inner, 12, LV_PART_MAIN);
    pulseRing = container;

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, container);
    lv_anim_set_exec_cb(&a, pulseExec);
    lv_anim_set_values(&a, 0, kPulseSteps);
    lv_anim_set_duration(&a, 900);
    lv_anim_set_completed_cb(&a, pulseCompleted);
    lv_anim_start(&a);
}

static void scaleExec(void *var, int32_t value) {
    lv_image_set_scale(static_cast<lv_obj_t *>(var), static_cast<uint32_t>(value));
}

void uiPopIn(lv_obj_t *image, uint32_t delayMs) {
    if (!image) {
        return;
    }
    lv_image_set_scale(image, 200);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, image);
    lv_anim_set_exec_cb(&a, scaleExec);
    lv_anim_set_values(&a, 200, LV_SCALE_NONE);
    lv_anim_set_duration(&a, 320);
    lv_anim_set_delay(&a, delayMs);
    lv_anim_set_path_cb(&a, lv_anim_path_overshoot);
    lv_anim_start(&a);
}

static void bgOpaExec(void *var, int32_t value) {
    lv_obj_set_style_bg_opa(static_cast<lv_obj_t *>(var), static_cast<lv_opa_t>(value), LV_PART_MAIN);
}

void uiBreathe(lv_obj_t *obj) {
    if (!obj || lv_anim_get(obj, bgOpaExec)) {
        return;
    }
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_exec_cb(&a, bgOpaExec);
    lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_20);
    lv_anim_set_duration(&a, 900);
    lv_anim_set_reverse_duration(&a, 900);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);
}

void uiBreatheStop(lv_obj_t *obj) {
    if (!obj || !lv_anim_get(obj, bgOpaExec)) {
        return;
    }
    lv_anim_delete(obj, bgOpaExec);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_MAIN);
}
