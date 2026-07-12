#include "device_setup_screen.h"

#include <Arduino.h>
#include <Preferences.h>
#include <cstring>

#include "device/device_identity.h"
#include "network/network_manager.h"
#include "sensors/sensor_mode.h"
#include "../../theme/ui_theme.h"

namespace device_setup_screen {
namespace {

lv_obj_t* deviceIdInput = nullptr;
lv_obj_t* statusLabel = nullptr;
lv_obj_t* keyboard = nullptr;
lv_obj_t* realModeButton = nullptr;
lv_obj_t* demoModeButton = nullptr;
lv_obj_t* lightModeButton = nullptr;
lv_obj_t* darkModeButton = nullptr;
lv_obj_t* autoModeButton = nullptr;
lv_obj_t* resetSetupCheckbox = nullptr;
lv_obj_t* resetWifiCheckbox = nullptr;
lv_obj_t* resetCalibrationCheckbox = nullptr;
lv_obj_t* resetUsageCheckbox = nullptr;
lv_obj_t* saveButton = nullptr;
lv_obj_t* saveButtonLabel = nullptr;
lv_obj_t* resetButton = nullptr;
sensor_mode::Mode pendingSensorMode = sensor_mode::Mode::Demo;
ui_theme::Mode pendingAppearance = ui_theme::Mode::Auto;

void selectSegment(lv_obj_t* selected, lv_obj_t* first, lv_obj_t* second, lv_obj_t* third = nullptr, lv_obj_t* fourth = nullptr);
void updateActionState();

bool checkboxChecked(lv_obj_t* checkbox) {
    return lv_obj_has_state(checkbox, LV_STATE_CHECKED);
}

bool clearPreferences(const char* name) {
    Preferences prefs;
    if (!prefs.begin(name, false)) return false;
    const bool cleared = prefs.clear();
    prefs.end();
    return cleared;
}

void restartTimerCb(lv_timer_t* timer) {
    lv_timer_del(timer);
    ESP.restart();
}

void restartWithFeedback(const char* message) {
    lv_label_set_text(saveButtonLabel, message);
    lv_obj_add_state(saveButton, LV_STATE_DISABLED);
    lv_obj_add_state(resetButton, LV_STATE_DISABLED);
    lv_timer_t* timer = lv_timer_create(restartTimerCb, 350, nullptr);
    lv_timer_set_repeat_count(timer, 1);
}

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

bool hasChanges() {
    return std::strcmp(lv_textarea_get_text(deviceIdInput), device_identity::getDeviceId()) != 0 ||
           pendingSensorMode != sensor_mode::get() || pendingAppearance != ui_theme::mode() ||
           checkboxChecked(resetSetupCheckbox) || checkboxChecked(resetWifiCheckbox) ||
           checkboxChecked(resetCalibrationCheckbox);
}

void updateActionState() {
    const bool dirty = hasChanges();
    if (dirty) {
        lv_obj_clear_state(saveButton, LV_STATE_DISABLED);
        lv_obj_clear_state(resetButton, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(saveButton, LV_STATE_DISABLED);
        lv_obj_add_state(resetButton, LV_STATE_DISABLED);
    }
}

void inputChangedCb(lv_event_t*) {
    lv_label_set_text(statusLabel, "");
    updateActionState();
}

void saveChangesCb(lv_event_t*) {
    const char* requestedId = lv_textarea_get_text(deviceIdInput);
    if (!requestedId || requestedId[0] == '\0') {
        lv_label_set_text(statusLabel, "Use lowercase letters, numbers, and hyphens (max 31).\nIt cannot start or end with a hyphen.");
        return;
    }

    const bool hostnameChanged = std::strcmp(requestedId, device_identity::getDeviceId()) != 0;
    const bool sensorModeChanged = pendingSensorMode != sensor_mode::get();
    const bool appearanceChanged = pendingAppearance != ui_theme::mode();
    const bool resetSetup = checkboxChecked(resetSetupCheckbox);
    const bool resetWifi = checkboxChecked(resetWifiCheckbox);
    const bool resetCalibration = checkboxChecked(resetCalibrationCheckbox);
    if (!hostnameChanged && !sensorModeChanged && !appearanceChanged && !resetSetup && !resetWifi && !resetCalibration) return;

    if (resetSetup) {
        if (!clearPreferences("device") || !clearPreferences("sensors") ||
            !clearPreferences("appearance")) {
            lv_label_set_text(statusLabel, "Could not reset setup preferences.");
            return;
        }
    } else {
        if (hostnameChanged && !device_identity::setDeviceId(requestedId)) {
            lv_label_set_text(statusLabel, "Use lowercase letters, numbers, and hyphens (max 31).\nIt cannot start or end with a hyphen.");
            return;
        }
        if (sensorModeChanged && !sensor_mode::set(pendingSensorMode)) {
            lv_label_set_text(statusLabel, "Could not save sensor mode.");
            return;
        }
        if (appearanceChanged) ui_theme::setMode(pendingAppearance);
    }

    if (resetCalibration && !clearPreferences("sensor_cal")) {
        lv_label_set_text(statusLabel, "Could not reset sensor calibration.");
        return;
    }

    if (resetWifi && !network_manager::clearSavedCredentials()) {
        lv_label_set_text(statusLabel, "Could not reset Wi-Fi credentials.");
        return;
    }

    // Hostname/mDNS, sensor mode, calibration, and the LVGL theme are all
    // initialized at boot. Save the page as one transaction, then restart.
    restartWithFeedback("Applying changes...");
}

void sensorModeChangedCb(lv_event_t* event) {
    pendingSensorMode = lv_event_get_target(event) == realModeButton ? sensor_mode::Mode::Real : sensor_mode::Mode::Demo;
    selectSegment(pendingSensorMode == sensor_mode::Mode::Real ? realModeButton : demoModeButton, realModeButton, demoModeButton);
    updateActionState();
}

void appearanceChangedCb(lv_event_t* event) {
    pendingAppearance = ui_theme::Mode::Auto;
    lv_obj_t* target = static_cast<lv_obj_t*>(lv_event_get_target(event));
    if (target == lightModeButton) pendingAppearance = ui_theme::Mode::Light;
    else if (target == darkModeButton) pendingAppearance = ui_theme::Mode::Dark;
    lv_obj_t* selected = pendingAppearance == ui_theme::Mode::Light ? lightModeButton : pendingAppearance == ui_theme::Mode::Dark ? darkModeButton : autoModeButton;
    selectSegment(selected, lightModeButton, darkModeButton, autoModeButton);
    updateActionState();
}

void resetOptionChangedCb(lv_event_t*) { updateActionState(); }

void resetChangesCb(lv_event_t*) {
    lv_textarea_set_text(deviceIdInput, device_identity::getDeviceId());
    pendingSensorMode = sensor_mode::get();
    pendingAppearance = ui_theme::mode();
    selectSegment(pendingSensorMode == sensor_mode::Mode::Real ? realModeButton : demoModeButton, realModeButton, demoModeButton);
    lv_obj_t* selected = pendingAppearance == ui_theme::Mode::Light ? lightModeButton : pendingAppearance == ui_theme::Mode::Dark ? darkModeButton : autoModeButton;
    selectSegment(selected, lightModeButton, darkModeButton, autoModeButton);
    lv_obj_clear_state(resetSetupCheckbox, LV_STATE_CHECKED);
    lv_obj_clear_state(resetWifiCheckbox, LV_STATE_CHECKED);
    lv_obj_clear_state(resetCalibrationCheckbox, LV_STATE_CHECKED);
    lv_label_set_text(statusLabel, "");
    updateActionState();
}

void selectSegment(lv_obj_t* selected, lv_obj_t* first, lv_obj_t* second, lv_obj_t* third, lv_obj_t* fourth) {
    lv_obj_clear_state(first, LV_STATE_CHECKED);
    lv_obj_clear_state(second, LV_STATE_CHECKED);
    if (third) lv_obj_clear_state(third, LV_STATE_CHECKED);
    if (fourth) lv_obj_clear_state(fourth, LV_STATE_CHECKED);
    lv_obj_add_state(selected, LV_STATE_CHECKED);
}

lv_obj_t* createSegmentGroup(lv_obj_t* parent, lv_coord_t height = 34) {
    lv_obj_t* group = lv_obj_create(parent);
    ui_theme::styleCard(group, 0);
    lv_obj_set_size(group, lv_pct(100), height);
    lv_obj_set_flex_flow(group, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(group, 0, 0);
    lv_obj_set_scrollbar_mode(group, LV_SCROLLBAR_MODE_OFF);
    return group;
}

void styleGroupedSegment(lv_obj_t* button) {
    ui_theme::styleSegment(button);
    // The group supplies the shared outline and rounded corners. Removing
    // per-button borders/radii makes this read as one segmented control.
    lv_obj_set_style_border_width(button, 0, 0);
    lv_obj_set_style_radius(button, 0, 0);
}

} // namespace

lv_obj_t* create(lv_obj_t* parent) {
    lv_obj_t* screen = lv_obj_create(parent);
    ui_theme::styleScreen(screen, 4);
    lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(screen, 5, 0);

    lv_obj_t* modeLabel = lv_label_create(screen);
    lv_label_set_text(modeLabel, "SENSOR MODE");
    ui_theme::styleSectionLabel(modeLabel);
    lv_obj_t* modeRow = createSegmentGroup(screen);
    realModeButton = lv_btn_create(modeRow); demoModeButton = lv_btn_create(modeRow);
    lv_obj_set_flex_grow(realModeButton, 1); lv_obj_set_flex_grow(demoModeButton, 1);
    styleGroupedSegment(realModeButton); styleGroupedSegment(demoModeButton);
    lv_label_set_text(lv_label_create(realModeButton), "Real"); lv_label_set_text(lv_label_create(demoModeButton), "Demo");
    lv_obj_add_event_cb(realModeButton, sensorModeChangedCb, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(demoModeButton, sensorModeChangedCb, LV_EVENT_CLICKED, nullptr);
    selectSegment(sensor_mode::get() == sensor_mode::Mode::Real ? realModeButton : demoModeButton, realModeButton, demoModeButton);

    lv_obj_t* appearanceLabel = lv_label_create(screen);
    lv_label_set_text(appearanceLabel, "APPEARANCE");
    ui_theme::styleSectionLabel(appearanceLabel);
    lv_obj_t* appearanceRow = createSegmentGroup(screen);
    lightModeButton = lv_btn_create(appearanceRow); darkModeButton = lv_btn_create(appearanceRow); autoModeButton = lv_btn_create(appearanceRow);
    lv_obj_set_flex_grow(lightModeButton, 1); lv_obj_set_flex_grow(darkModeButton, 1); lv_obj_set_flex_grow(autoModeButton, 1);
    styleGroupedSegment(lightModeButton); styleGroupedSegment(darkModeButton); styleGroupedSegment(autoModeButton);
    lv_label_set_text(lv_label_create(lightModeButton), "Light"); lv_label_set_text(lv_label_create(darkModeButton), "Dark"); lv_label_set_text(lv_label_create(autoModeButton), "Auto");
    lv_obj_add_event_cb(lightModeButton, appearanceChangedCb, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(darkModeButton, appearanceChangedCb, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(autoModeButton, appearanceChangedCb, LV_EVENT_CLICKED, nullptr);
    pendingSensorMode = sensor_mode::get();
    pendingAppearance = ui_theme::mode();
    lv_obj_t* activeAppearance = pendingAppearance == ui_theme::Mode::Light ? lightModeButton : pendingAppearance == ui_theme::Mode::Dark ? darkModeButton : autoModeButton;
    selectSegment(activeAppearance, lightModeButton, darkModeButton, autoModeButton);

    lv_obj_t* nameLabel = lv_label_create(screen);
    lv_label_set_text(nameLabel, "HOSTNAME");
    ui_theme::styleSectionLabel(nameLabel);

    deviceIdInput = lv_textarea_create(screen);
    lv_obj_set_width(deviceIdInput, lv_pct(100));
    lv_textarea_set_one_line(deviceIdInput, true);
    lv_textarea_set_max_length(deviceIdInput, 31);
    lv_textarea_set_text(deviceIdInput, device_identity::getDeviceId());
    lv_obj_add_event_cb(deviceIdInput, inputFocusCb, LV_EVENT_FOCUSED, nullptr);
    lv_obj_add_event_cb(deviceIdInput, inputDefocusCb, LV_EVENT_DEFOCUSED, nullptr);
    lv_obj_add_event_cb(deviceIdInput, inputChangedCb, LV_EVENT_VALUE_CHANGED, nullptr);

    statusLabel = lv_label_create(screen);
    lv_obj_set_width(statusLabel, lv_pct(100));
    lv_label_set_text(statusLabel, "");

    lv_obj_t* resetLabel = lv_label_create(screen);
    lv_label_set_text(resetLabel, "RESET");
    ui_theme::styleSectionLabel(resetLabel);
    resetSetupCheckbox = lv_checkbox_create(screen);
    lv_checkbox_set_text(resetSetupCheckbox, "Setup");
    lv_obj_add_event_cb(resetSetupCheckbox, resetOptionChangedCb, LV_EVENT_VALUE_CHANGED, nullptr);
    resetWifiCheckbox = lv_checkbox_create(screen);
    lv_checkbox_set_text(resetWifiCheckbox, "Wi-Fi");
    lv_obj_add_event_cb(resetWifiCheckbox, resetOptionChangedCb, LV_EVENT_VALUE_CHANGED, nullptr);
    resetCalibrationCheckbox = lv_checkbox_create(screen);
    lv_checkbox_set_text(resetCalibrationCheckbox, "Sensor Calibration");
    lv_obj_add_event_cb(resetCalibrationCheckbox, resetOptionChangedCb, LV_EVENT_VALUE_CHANGED, nullptr);
    resetUsageCheckbox = lv_checkbox_create(screen);
    lv_checkbox_set_text(resetUsageCheckbox, "Usage (coming soon)");
    lv_obj_add_state(resetUsageCheckbox, LV_STATE_DISABLED);

    lv_obj_t* actionRow = lv_obj_create(screen);
    lv_obj_remove_style_all(actionRow);
    lv_obj_set_size(actionRow, lv_pct(100), 34);
    lv_obj_set_flex_flow(actionRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(actionRow, 5, 0);

    resetButton = lv_btn_create(actionRow);
    lv_obj_set_flex_grow(resetButton, 1);
    lv_obj_set_style_bg_opa(resetButton, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(resetButton, ui_theme::border(), 0);
    lv_obj_set_style_border_width(resetButton, 1, 0);
    lv_obj_set_style_radius(resetButton, 6, 0);
    lv_obj_set_style_text_color(resetButton, ui_theme::mutedText(), 0);
    lv_obj_set_style_opa(resetButton, LV_OPA_50, LV_STATE_DISABLED);
    lv_obj_add_event_cb(resetButton, resetChangesCb, LV_EVENT_CLICKED, nullptr);
    lv_label_set_text(lv_label_create(resetButton), "Discard");

    saveButton = lv_btn_create(actionRow);
    lv_obj_set_flex_grow(saveButton, 1);
    lv_obj_add_event_cb(saveButton, saveChangesCb, LV_EVENT_CLICKED, nullptr);
    ui_theme::stylePrimaryButton(saveButton);
    saveButtonLabel = lv_label_create(saveButton);
    lv_label_set_text(saveButtonLabel, "Save");
    updateActionState();

    keyboard = lv_keyboard_create(screen);
    lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(keyboard, keyboardEventCb, LV_EVENT_ALL, nullptr);

    return screen;
}

} // namespace device_setup_screen
