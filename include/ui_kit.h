#pragma once

#include <lvgl.h>

// FPL Buddy design system: colour/type tokens, round-display layout helpers and the shared components the
// screens are built from. LVGL objects only - no FPL data in here. All functions must run on the UI task.

// --- Fonts (src/fonts, Montserrat SemiBold/Bold/Medium + a few Font Awesome glyphs) ---------------------------
LV_FONT_DECLARE(ui_font_hero_52);     // SemiBold, digits and " #+,-.:kM" only
LV_FONT_DECLARE(ui_font_value_30);    // SemiBold
LV_FONT_DECLARE(ui_font_name_22);     // SemiBold
LV_FONT_DECLARE(ui_font_body_19);     // Medium
LV_FONT_DECLARE(ui_font_meta_16);     // Medium
LV_FONT_DECLARE(ui_font_heading_18);  // Bold
LV_FONT_DECLARE(ui_font_pill_15);     // Bold

#define UI_FONT_HERO (&ui_font_hero_52)          // the one number a screen is about
#define UI_FONT_VALUE (&ui_font_value_30)        // primary values
#define UI_FONT_NAME (&ui_font_name_22)          // player / league names, row values
#define UI_FONT_BODY (&ui_font_body_19)          // secondary information
#define UI_FONT_META (&ui_font_meta_16)          // labels and metadata (nothing important goes smaller)
#define UI_FONT_HEADING (&ui_font_heading_18)    // screen headings, upper case
#define UI_FONT_PILL (&ui_font_pill_15)          // status pills, upper case

// Glyphs available in every UI_FONT_* except the hero font (UTF-8).
#define UI_SYM_UP "\xEF\x83\x98"     // caret up (Font Awesome U+F0D8)
#define UI_SYM_DOWN "\xEF\x83\x97"   // caret down (U+F0D7)
#define UI_SYM_ARROW "\xEF\x81\xA1"  // arrow right (U+F061)
#define UI_SYM_CHECK "\xEF\x80\x8C"  // check (U+F00C)
#define UI_POUND "\xC2\xA3"
#define UI_DOT "\xC2\xB7"
#define UI_DASH "\xE2\x80\x94"

// --- Colour tokens --------------------------------------------------------------------------------------------
static constexpr uint32_t kUiBg = 0x1A0533;             // screen background (the kit images are drawn on it)
static constexpr uint32_t kUiSurface = 0x2B1850;        // cards / rows
static constexpr uint32_t kUiSurfaceRaised = 0x3A2566;  // neutral pills, inactive page dots
static constexpr uint32_t kUiTextPrimary = 0xFFFFFF;
static constexpr uint32_t kUiTextSecondary = 0xB6A6CC;
static constexpr uint32_t kUiAccent = 0x00E5FF;
static constexpr uint32_t kUiPositive = 0x00FF87;
static constexpr uint32_t kUiNegative = 0xFF6B6B;
static constexpr uint32_t kUiWarning = 0xFFC107;

// Colour carries meaning only: rises/climbs are Positive, falls/drops Negative, "nothing happening" Neutral.
enum class UiTone : uint8_t { Neutral, Accent, Positive, Negative, Warning };
uint32_t uiToneColor(UiTone tone);

// --- Round display layout -------------------------------------------------------------------------------------
static constexpr int kUiScreenSize = 412;
static constexpr int kUiHeaderY = 34;        // top of the screen heading
static constexpr int kUiDotsY = 372;         // centre of the page dots
static constexpr int kUiMotionMs = 220;      // page transitions
static constexpr int kUiNumberMs = 350;      // animated number changes

// Widest content that fits inside the circle across the horizontal band [cy - h/2, cy + h/2], where cy is
// measured from the screen centre, less `margin` on each side. Content near the centre can be wide; content
// near the top and bottom has to be narrower.
int uiSafeWidth(int cy, int height, int margin = 12);

// --- Components -----------------------------------------------------------------------------------------------
lv_obj_t *uiLabel(lv_obj_t *parent, const lv_font_t *font, uint32_t color,
                  lv_text_align_t align = LV_TEXT_ALIGN_CENTER);
// Keeps a label to one line of its width, ending in "..." when the text is longer. (LVGL only truncates a label
// whose height is fixed; otherwise it wraps.) Set the width, or a flex grow, separately.
void uiSingleLine(lv_obj_t *label);
// Sets the label text only when it differs, so unchanged labels are not redrawn.
void uiSetText(lv_obj_t *label, const char *text);
// Sets a label's text colour only when it differs (restyling redraws the object even when nothing changed).
void uiSetTextColor(lv_obj_t *label, uint32_t color);
// A transparent, non-clickable, non-scrollable container (for grouping and alignment).
lv_obj_t *uiBox(lv_obj_t *parent, int w, int h);

// Upper-case heading at the top of the screen, with an optional line of metadata under it.
void uiScreenHeader(lv_obj_t *parent, const char *title, const char *subtitle = nullptr);
// Content-sized rounded pill, e.g. "LIKELY RISE". Position it with lv_obj_align / lv_obj_align_to.
lv_obj_t *uiStatusPill(lv_obj_t *parent, const char *text, UiTone tone, const lv_font_t *font = UI_FONT_PILL);
// Replaces a pill's text and tone in place.
void uiStatusPillSet(lv_obj_t *pill, const char *text, UiTone tone);
// Surface-coloured list row centred at vertical offset `cy` from the screen centre.
lv_obj_t *uiListRow(lv_obj_t *parent, int cy, int w, int h);
// Centre layout for rows: the centre offset of row `index` of `count` rows of height `h` with `gap` between
// them, the block centred on `blockCy`.
int uiRowCenter(size_t index, size_t count, int h, int gap, int blockCy);
// Ring icon + title + one line of explanation, centred on the screen.
void uiEmptyState(lv_obj_t *parent, const char *icon, const char *title, const char *body,
                  UiTone tone = UiTone::Accent);
// Carousel position indicator near the bottom edge; `count` <= 1 draws nothing.
void uiPageDots(lv_obj_t *parent, uint8_t count, uint8_t active);
// Small filled circle (status dot, chart end point).
lv_obj_t *uiDot(lv_obj_t *parent, int size, uint32_t color);
// Battery icon centred under the page dots, on the top layer so it stays put while pages slide. Hidden until
// the first uiBatterySet.
void uiBatteryCreate();
// Fill follows `percent`; the icon turns amber at 30 %, red at 20 % and blinks red at 10 %.
void uiBatterySet(int percent);

// --- Motion ---------------------------------------------------------------------------------------------------
// A label that shows an integer with thousands separators and counts from its old value to a new one.
void uiNumberInit(lv_obj_t *label);
void uiNumberSet(lv_obj_t *label, int32_t value, bool animate);
void uiNumberClear(lv_obj_t *label, const char *placeholder);
// Fades an object in from transparent.
void uiFadeIn(lv_obj_t *obj, uint32_t durationMs = kUiMotionMs, uint32_t delayMs = 0);
// Covers the area with the background colour and wipes the cover away left to right, so whatever is
// underneath (a chart line) appears to be drawn. Create it after the content it reveals.
void uiRevealLeftToRight(lv_obj_t *parent, int x, int y, int w, int h, uint32_t durationMs, uint32_t delayMs = 0);
// A short glow around the edge of the display: "new FPL data just arrived" (cyan), or a points event in its
// tone colour.
void uiPulseRing(uint32_t color = kUiAccent);
// Image "pops" in: scales up from slightly small with a little overshoot.
void uiPopIn(lv_obj_t *image, uint32_t delayMs = 0);
// Slow, endless fade of a small object's background (the "live" dot). uiBreatheStop leaves it fully opaque.
void uiBreathe(lv_obj_t *obj);
void uiBreatheStop(lv_obj_t *obj);

void uiFormatThousands(int32_t value, char *out, size_t outLen);
