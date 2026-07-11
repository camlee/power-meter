#include "screen_setup.h"

#include <Arduino.h>

#include "device/device_identity.h"
#include "sensors/sensor_mode.h"

namespace screen_setup {
namespace {

lv_obj_t* deviceIdInput = nullptr;
lv_obj_t* statusLabel = nullptr;
lv_obj_t* keyboard = nullptr;
lv_obj_t* realModeButton = nullptr;
lv_obj_t* demoModeButton = nullptr;

void hideKeyboard() {
    if (keyboard) lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
}

void inputFocusCb(lv_event_t* event) {
    lv_keyboard_set_textarea(keyboard, static_cast<lv_obj_t*>(lv_event_get_target(event)));
    lv_obj_clear_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
}

void inputDefocusCb(lv_event_t*) { hideKeyboard(); }

void keyboardEventCb(lv_event_t* event) {
    const lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL) hideKeyboard();
}

void saveDeviceIdCb(lv_event_t*) {
    const char* requestedId = lv_textarea_get_text(deviceIdInput);
    if (!device_identity::setDeviceId(requestedId)) {
        lv_label_set_text(statusLabel, "Use lowercase letters, numbers, and hyphens (max 31).\nIt cannot start or end with a hyphen.");
        return;
    }

    // Network hostname and mDNS are established at boot. Restarting applies
    // the new persisted ID to station and AP networking consistently.
    lv_label_set_text(statusLabel, "Saved. Restarting to apply hostname...");
    delay(250);
    ESP.restart();
}

void sensorModeChangedCb(lv_event_t* event) {
    const auto mode = lv_event_get_target(event) == realModeButton ? sensor_mode::Mode::Real : sensor_mode::Mode::Demo;
    if (!sensor_mode::set(mode)) { lv_label_set_text(statusLabel, "Could not save sensor mode."); return; }
    lv_label_set_text(statusLabel, "Sensor mode saved. Restarting...");
    delay(250); ESP.restart();
}

} // namespace

lv_obj_t* create(lv_obj_t* parent) {
    lv_obj_t* screen = lv_obj_create(parent);
    lv_obj_remove_style_all(screen);
    lv_obj_set_size(screen, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(screen, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(screen, 3, 0);
    lv_obj_set_style_border_width(screen, 0, 0);
    lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(screen, 3, 0);

    lv_obj_t* title = lv_label_create(screen);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_label_set_text(title, "Setup");

    lv_obj_t* modeLabel = lv_label_create(screen);
    lv_label_set_text(modeLabel, "sensor mode");
    lv_obj_t* modeRow = lv_obj_create(screen);
    lv_obj_remove_style_all(modeRow); lv_obj_set_width(modeRow, lv_pct(100));
    lv_obj_set_flex_flow(modeRow, LV_FLEX_FLOW_ROW); lv_obj_set_style_pad_column(modeRow, 8, 0);
    realModeButton = lv_btn_create(modeRow); demoModeButton = lv_btn_create(modeRow);
    lv_obj_set_flex_grow(realModeButton, 1); lv_obj_set_flex_grow(demoModeButton, 1);
    lv_label_set_text(lv_label_create(realModeButton), "Real"); lv_label_set_text(lv_label_create(demoModeButton), "Demo");
    lv_obj_add_event_cb(realModeButton, sensorModeChangedCb, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(demoModeButton, sensorModeChangedCb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* nameLabel = lv_label_create(screen);
    lv_label_set_text(nameLabel, "hostname");

    deviceIdInput = lv_textarea_create(screen);
    lv_obj_set_width(deviceIdInput, lv_pct(100));
    lv_textarea_set_one_line(deviceIdInput, true);
    lv_textarea_set_max_length(deviceIdInput, 31);
    lv_textarea_set_text(deviceIdInput, device_identity::getDeviceId());
    lv_obj_add_event_cb(deviceIdInput, inputFocusCb, LV_EVENT_FOCUSED, nullptr);
    lv_obj_add_event_cb(deviceIdInput, inputDefocusCb, LV_EVENT_DEFOCUSED, nullptr);

    lv_obj_t* saveButton = lv_btn_create(screen);
    lv_obj_set_width(saveButton, lv_pct(100));
    lv_obj_add_event_cb(saveButton, saveDeviceIdCb, LV_EVENT_CLICKED, nullptr);
    lv_label_set_text(lv_label_create(saveButton), "Save and restart");

    statusLabel = lv_label_create(screen);
    lv_obj_set_width(statusLabel, lv_pct(100));
    lv_label_set_text(statusLabel, "");

    keyboard = lv_keyboard_create(screen);
    lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(keyboard, keyboardEventCb, LV_EVENT_ALL, nullptr);

    return screen;
}

} // namespace screen_setup
