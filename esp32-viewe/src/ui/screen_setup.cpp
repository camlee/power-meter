#include "screen_setup.h"

#include <Arduino.h>

#include "device/device_identity.h"

namespace screen_setup {
namespace {

lv_obj_t* deviceIdInput = nullptr;
lv_obj_t* statusLabel = nullptr;
lv_obj_t* keyboard = nullptr;

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

} // namespace

lv_obj_t* create(lv_obj_t* parent) {
    lv_obj_t* screen = lv_obj_create(parent);
    lv_obj_set_size(screen, lv_pct(100), lv_pct(100));
    lv_obj_set_style_pad_all(screen, 12, 0);
    lv_obj_set_style_border_width(screen, 0, 0);
    lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(screen, 10, 0);

    lv_obj_t* title = lv_label_create(screen);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_label_set_text(title, "Setup");

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
