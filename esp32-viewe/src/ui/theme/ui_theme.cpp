#include "ui_theme.h"

#include <Preferences.h>
#include <time.h>

namespace ui_theme {
namespace {
Mode selectedMode = Mode::Auto;
bool appliedDark = true;
bool restartPending = false;

constexpr char kPreferencesNamespace[] = "appearance";
constexpr char kModeKey[] = "mode";
constexpr char kAutoDarkKey[] = "auto_dark";

bool clockIsValid() {
    time_t now = time(nullptr);
    struct tm local {};
    localtime_r(&now, &local);
    return local.tm_year >= (2020 - 1900);
}

bool desiredAutoDark() {
    time_t now = time(nullptr);
    struct tm local {};
    localtime_r(&now, &local);
    return local.tm_hour >= 19 || local.tm_hour < 7;
}

lv_color_t color(uint32_t light, uint32_t dark) { return lv_color_hex(isDark() ? dark : light); }
}

void init() {
    Preferences prefs;
    bool savedAutoDark = true;
    if (prefs.begin(kPreferencesNamespace, true)) {
        const uint8_t value = prefs.getUChar(kModeKey, static_cast<uint8_t>(Mode::Auto));
        selectedMode = value <= static_cast<uint8_t>(Mode::Auto) ? static_cast<Mode>(value) : Mode::Auto;
        savedAutoDark = prefs.getBool(kAutoDarkKey, true);
        prefs.end();
    }

    // Before the first trustworthy clock arrives, reuse Auto's last applied
    // appearance. This prevents a daytime NTP sync from causing a reboot loop.
    appliedDark = selectedMode == Mode::Dark ||
        (selectedMode == Mode::Auto && (clockIsValid() ? desiredAutoDark() : savedAutoDark));
    restartPending = false;

    lv_theme_t* theme = lv_theme_default_init(nullptr, accent(), accent(), appliedDark, LV_FONT_DEFAULT);
    if (lv_disp_get_default()) lv_disp_set_theme(lv_disp_get_default(), theme);
}

Mode mode() { return selectedMode; }
bool isDark() { return appliedDark; }

void setMode(Mode value) {
    Preferences prefs;
    if (prefs.begin(kPreferencesNamespace, false)) {
        prefs.putUChar(kModeKey, static_cast<uint8_t>(value));
        prefs.end();
    }
    selectedMode = value;
}

bool autoRestartRequired() {
    if (restartPending) return true;
    if (selectedMode != Mode::Auto || !clockIsValid()) return false;

    const bool desiredDark = desiredAutoDark();
    if (desiredDark == appliedDark) return false;

    // Save before restarting so boot can apply the intended appearance even
    // if network time is not restored until after LVGL is created.
    Preferences prefs;
    bool saved = false;
    if (prefs.begin(kPreferencesNamespace, false)) {
        saved = prefs.putBool(kAutoDarkKey, desiredDark) == sizeof(bool);
        prefs.end();
    }
    if (!saved) return false;
    restartPending = true;
    return true;
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
