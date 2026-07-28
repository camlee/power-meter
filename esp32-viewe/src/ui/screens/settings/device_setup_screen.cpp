#include "device_setup_screen.h"

#include <Arduino.h>
#include <Preferences.h>
#include <cstring>

#include "device/device_identity.h"
#include "device/hardware_profile.h"
#include "data/historical_storage.h"
#include "network/network_manager.h"
#include "sensors/sensor_mode.h"
#include "sensors/sensors.h"
#include "../../theme/ui_theme.h"

namespace device_setup_screen {
namespace {

lv_obj_t* deviceIdInput = nullptr;
lv_obj_t* statusLabel = nullptr;
lv_obj_t* keyboard = nullptr;
lv_obj_t* adcModeButton = nullptr;
lv_obj_t* ads1115ModeButton = nullptr;
lv_obj_t* uartModeButton = nullptr;
lv_obj_t* demoModeButton = nullptr;
lv_obj_t* activeSensorStatusLabel = nullptr;
lv_obj_t* screenObject = nullptr;
lv_obj_t* hostnameLabel = nullptr;
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
lv_coord_t setupScrollY = 0;
bool hostnameEditing = false;
sensor_mode::Mode pendingSensorMode = sensor_mode::Mode::Demo;
ui_theme::Mode pendingAppearance = ui_theme::Mode::Auto;

void selectSegment(lv_obj_t* selected, lv_obj_t* first, lv_obj_t* second, lv_obj_t* third = nullptr, lv_obj_t* fourth = nullptr);
void updateActionState();

const char* sensorModeLabel(sensor_mode::Mode mode) {
    switch (mode) {
        case sensor_mode::Mode::Adc: return "ADC";
        case sensor_mode::Mode::Ads1115: return "ADS1115";
        case sensor_mode::Mode::Uart: return "UART";
        case sensor_mode::Mode::Demo: return "Demo";
    }
    return "Sensor";
}

const char* readingSymbol(sensors::SensorId id) {
    sensors::Reading reading;
    if (!sensors::getLatest(id, reading)) return LV_SYMBOL_CLOSE;
    switch (reading.state) {
        case sensors::ReadingState::Valid: return LV_SYMBOL_OK;
        case sensors::ReadingState::OutOfRange: return LV_SYMBOL_WARNING;
        case sensors::ReadingState::Waiting:
        case sensors::ReadingState::NotConfigured:
        case sensors::ReadingState::Invalid:
        case sensors::ReadingState::Stale: return LV_SYMBOL_CLOSE;
    }
    return LV_SYMBOL_CLOSE;
}

void updateActiveSensorStatus() {
    if (!activeSensorStatusLabel) return;
    char text[96];
    snprintf(text, sizeof(text), "%s  In %s  Out %s  Aux %s",
             sensorModeLabel(sensor_mode::get()), readingSymbol(sensors::SENSOR_IN),
             readingSymbol(sensors::SENSOR_OUT), readingSymbol(sensors::SENSOR_AUX));
    lv_label_set_text(activeSensorStatusLabel, text);
    lv_obj_set_style_text_opa(activeSensorStatusLabel,
                              pendingSensorMode == sensor_mode::get() ? LV_OPA_COVER : LV_OPA_20,
                              0);
}

void screenRefreshCb(lv_event_t* event) {
    if (lv_event_get_code(event) == LV_EVENT_REFRESH) updateActiveSensorStatus();
}

void sensorStatusTimerCb(lv_timer_t*) {
    if (screenObject && lv_obj_is_visible(screenObject)) updateActiveSensorStatus();
}

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

void setHostnameEditing(bool editing) {
    if (!screenObject || !hostnameLabel || !deviceIdInput || !keyboard ||
        hostnameEditing == editing) return;

    hostnameEditing = editing;
    if (editing) setupScrollY = lv_obj_get_scroll_y(screenObject);

    const uint32_t childCount = lv_obj_get_child_cnt(screenObject);
    for (uint32_t i = 0; i < childCount; ++i) {
        lv_obj_t* child = lv_obj_get_child(screenObject, i);
        if (child == hostnameLabel || child == deviceIdInput || child == keyboard) continue;
        if (editing) lv_obj_add_flag(child, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_clear_flag(child, LV_OBJ_FLAG_HIDDEN);
    }

    if (editing) {
        lv_obj_clear_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_obj_update_layout(screenObject);
        lv_obj_scroll_to_y(screenObject, 0, LV_ANIM_OFF);
    } else {
        lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_obj_update_layout(screenObject);
        lv_obj_scroll_to_y(screenObject, setupScrollY, LV_ANIM_OFF);
    }
}

void hideKeyboard() { setHostnameEditing(false); }

void inputFocusCb(lv_event_t* event) {
    lv_keyboard_set_textarea(keyboard, static_cast<lv_obj_t*>(lv_event_get_target(event)));
    setHostnameEditing(true);
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
           checkboxChecked(resetCalibrationCheckbox) || checkboxChecked(resetUsageCheckbox);
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
    const bool resetUsage = checkboxChecked(resetUsageCheckbox);
    if (!hostnameChanged && !sensorModeChanged && !appearanceChanged && !resetSetup && !resetWifi &&
        !resetCalibration && !resetUsage) return;

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

    if (resetUsage && !historical_storage::clearAll()) {
        lv_label_set_text(statusLabel, "Could not reset usage history.");
        return;
    }

    // Hostname/mDNS, sensor mode, calibration, and the LVGL theme are all
    // initialized at boot. Save the page as one transaction, then restart.
    restartWithFeedback("Applying changes...");
}

void sensorModeChangedCb(lv_event_t* event) {
    lv_obj_t* target = lv_event_get_target(event);
    pendingSensorMode = target == adcModeButton ? sensor_mode::Mode::Adc :
                        target == ads1115ModeButton ? sensor_mode::Mode::Ads1115 :
                        target == uartModeButton ? sensor_mode::Mode::Uart : sensor_mode::Mode::Demo;
    selectSegment(target, adcModeButton, ads1115ModeButton, uartModeButton, demoModeButton);
    updateActiveSensorStatus();
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
    lv_obj_t* activeSensor = pendingSensorMode == sensor_mode::Mode::Adc ? adcModeButton :
                             pendingSensorMode == sensor_mode::Mode::Ads1115 ? ads1115ModeButton :
                             pendingSensorMode == sensor_mode::Mode::Uart ? uartModeButton : demoModeButton;
    selectSegment(activeSensor, adcModeButton, ads1115ModeButton, uartModeButton, demoModeButton);
    updateActiveSensorStatus();
    lv_obj_t* selected = pendingAppearance == ui_theme::Mode::Light ? lightModeButton : pendingAppearance == ui_theme::Mode::Dark ? darkModeButton : autoModeButton;
    selectSegment(selected, lightModeButton, darkModeButton, autoModeButton);
    lv_obj_clear_state(resetSetupCheckbox, LV_STATE_CHECKED);
    lv_obj_clear_state(resetWifiCheckbox, LV_STATE_CHECKED);
    lv_obj_clear_state(resetCalibrationCheckbox, LV_STATE_CHECKED);
    lv_obj_clear_state(resetUsageCheckbox, LV_STATE_CHECKED);
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
    screenObject = screen;
    ui_theme::styleScreen(screen, 4);
    lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(screen, 5, 0);

    lv_obj_t* modeLabel = lv_label_create(screen);
    lv_label_set_text(modeLabel, "SENSOR MODE");
    ui_theme::styleSectionLabel(modeLabel);
    lv_obj_t* modeRow = createSegmentGroup(screen);
    adcModeButton = lv_btn_create(modeRow); ads1115ModeButton = lv_btn_create(modeRow);
    uartModeButton = lv_btn_create(modeRow); demoModeButton = lv_btn_create(modeRow);
    lv_obj_set_flex_grow(adcModeButton, 1); lv_obj_set_flex_grow(ads1115ModeButton, 1);
    lv_obj_set_flex_grow(uartModeButton, 1); lv_obj_set_flex_grow(demoModeButton, 1);
    styleGroupedSegment(adcModeButton); styleGroupedSegment(ads1115ModeButton);
    styleGroupedSegment(uartModeButton); styleGroupedSegment(demoModeButton);
    lv_label_set_text(lv_label_create(adcModeButton), "ADC");
    lv_label_set_text(lv_label_create(ads1115ModeButton), "ADS");
    lv_label_set_text(lv_label_create(uartModeButton), "UART");
    lv_label_set_text(lv_label_create(demoModeButton), "Demo");
    lv_obj_add_event_cb(adcModeButton, sensorModeChangedCb, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(ads1115ModeButton, sensorModeChangedCb, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(uartModeButton, sensorModeChangedCb, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(demoModeButton, sensorModeChangedCb, LV_EVENT_CLICKED, nullptr);
    if (!hardware_profile::kHasEsp32Adc) lv_obj_add_flag(adcModeButton, LV_OBJ_FLAG_HIDDEN);
    if (!hardware_profile::kHasAds1115) lv_obj_add_flag(ads1115ModeButton, LV_OBJ_FLAG_HIDDEN);
    if (!hardware_profile::kSupportsUart) lv_obj_add_flag(uartModeButton, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t* activeSensor = sensor_mode::get() == sensor_mode::Mode::Adc ? adcModeButton :
                             sensor_mode::get() == sensor_mode::Mode::Ads1115 ? ads1115ModeButton :
                             sensor_mode::get() == sensor_mode::Mode::Uart ? uartModeButton : demoModeButton;
    selectSegment(activeSensor, adcModeButton, ads1115ModeButton, uartModeButton, demoModeButton);

    activeSensorStatusLabel = lv_label_create(screen);
    lv_obj_set_width(activeSensorStatusLabel, lv_pct(100));
    lv_obj_set_style_text_align(activeSensorStatusLabel, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(activeSensorStatusLabel, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(activeSensorStatusLabel, ui_theme::mutedText(), 0);

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

    hostnameLabel = lv_label_create(screen);
    lv_label_set_text(hostnameLabel, "HOSTNAME");
    ui_theme::styleSectionLabel(hostnameLabel);

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
    lv_checkbox_set_text(resetUsageCheckbox, "Usage Data");
    lv_obj_add_event_cb(resetUsageCheckbox, resetOptionChangedCb, LV_EVENT_VALUE_CHANGED, nullptr);

    lv_obj_t* actionSpacer = lv_obj_create(screen);
    lv_obj_remove_style_all(actionSpacer);
    lv_obj_set_width(actionSpacer, lv_pct(100));
    lv_obj_set_flex_grow(actionSpacer, 1);

    lv_obj_t* actionRow = lv_obj_create(screen);
    lv_obj_remove_style_all(actionRow);
    lv_obj_set_size(actionRow, lv_pct(100), 34);
    lv_obj_set_flex_flow(actionRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(actionRow, 5, 0);
    lv_obj_set_flex_align(actionRow, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_END);

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
    updateActiveSensorStatus();
    updateActionState();

    keyboard = lv_keyboard_create(screen);
    lv_obj_set_size(keyboard, lv_pct(100), 0);
    lv_obj_set_flex_grow(keyboard, 1);
    lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(keyboard, keyboardEventCb, LV_EVENT_ALL, nullptr);
    lv_obj_add_event_cb(screen, screenRefreshCb, LV_EVENT_REFRESH, nullptr);
    lv_timer_create(sensorStatusTimerCb, 1000, nullptr);

    return screen;
}

} // namespace device_setup_screen
