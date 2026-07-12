#include "ui_theme.h"

#include <Preferences.h>
#include <time.h>

namespace ui_theme {
namespace {
Mode selectedMode = Mode::Auto;

bool clockIsValid() {
    time_t now = time(nullptr);
    struct tm local {};
    localtime_r(&now, &local);
    return local.tm_year >= (2020 - 1900);
}

bool autoDark() {
    // A newly booted offline meter has no trustworthy wall clock. Prefer the
    // safer night-friendly appearance until NTP becomes available.
    if (!clockIsValid()) return true;
    time_t now = time(nullptr);
    struct tm local {};
    localtime_r(&now, &local);
    return local.tm_hour >= 19 || local.tm_hour < 7;
}

lv_color_t color(uint32_t light, uint32_t dark) { return lv_color_hex(isDark() ? dark : light); }
}

void init() {
    Preferences prefs;
    if (prefs.begin("appearance", true)) {
        const uint8_t value = prefs.getUChar("mode", static_cast<uint8_t>(Mode::Auto));
        selectedMode = value <= static_cast<uint8_t>(Mode::Auto) ? static_cast<Mode>(value) : Mode::Auto;
        prefs.end();
    }

    lv_theme_t* theme = lv_theme_default_init(nullptr, accent(), accent(), isDark(), LV_FONT_DEFAULT);
    if (lv_disp_get_default()) lv_disp_set_theme(lv_disp_get_default(), theme);
}

Mode mode() { return selectedMode; }
bool isDark() { return selectedMode == Mode::Dark || (selectedMode == Mode::Auto && autoDark()); }

void setMode(Mode value) {
    Preferences prefs;
    if (prefs.begin("appearance", false)) {
        prefs.putUChar("mode", static_cast<uint8_t>(value));
        prefs.end();
    }
    selectedMode = value;
}

lv_color_t background() { return color(0xFFFFFF, 0x101417); }
lv_color_t surface() { return color(0xFFFFFF, 0x1A2025); }
lv_color_t surfaceAlt() { return color(0xF3F5F6, 0x242C33); }
lv_color_t text() { return color(0x202428, 0xF1F5F7); }
lv_color_t mutedText() { return color(0x7A838A, 0xAAB5BC); }
lv_color_t border() { return color(0xD8DDE0, 0x3C4851); }
lv_color_t accent() { return color(0x1688E8, 0x4CA9F5); }

void styleScreen(lv_obj_t* obj, lv_coord_t padding) {
    lv_obj_remove_style_all(obj);
    lv_obj_set_size(obj, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(obj, background(), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(obj, text(), 0);
    lv_obj_set_style_pad_all(obj, padding, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_OFF);
}

void styleCard(lv_obj_t* obj, lv_coord_t padding) {
    lv_obj_remove_style_all(obj);
    lv_obj_set_style_bg_color(obj, surface(), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(obj, border(), 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_set_style_radius(obj, 7, 0);
    lv_obj_set_style_pad_all(obj, padding, 0);
    lv_obj_set_style_text_color(obj, text(), 0);
}

void stylePrimaryButton(lv_obj_t* obj) {
    lv_obj_set_style_bg_color(obj, accent(), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_radius(obj, 6, 0);
    lv_obj_set_style_text_color(obj, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_40, LV_STATE_DISABLED);
    lv_obj_set_style_text_opa(obj, LV_OPA_60, LV_STATE_DISABLED);
}

void styleSegment(lv_obj_t* obj) {
    lv_obj_add_flag(obj, LV_OBJ_FLAG_CHECKABLE);
    lv_obj_set_style_bg_color(obj, surfaceAlt(), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(obj, border(), 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_set_style_radius(obj, 6, 0);
    lv_obj_set_style_text_color(obj, text(), 0);
    lv_obj_set_style_bg_color(obj, accent(), LV_STATE_CHECKED);
    lv_obj_set_style_border_color(obj, accent(), LV_STATE_CHECKED);
    lv_obj_set_style_text_color(obj, lv_color_white(), LV_STATE_CHECKED);
}

void styleSectionLabel(lv_obj_t* obj) {
    lv_obj_set_style_text_font(obj, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(obj, mutedText(), 0);
}

} // namespace ui_theme
