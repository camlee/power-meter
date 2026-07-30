#include "sensor_mapping_overlay.h"

#include <Arduino.h>
#include <cmath>
#include <cstring>
#include <lvgl.h>

#include "sensors/sensor_mapping.h"
#include "sensors/sensor_mode.h"
#include "sensors/sensors.h"
#include "../../theme/ui_theme.h"

namespace sensor_mapping_overlay {
namespace {

struct SensorRow {
    lv_obj_t* status = nullptr;
    lv_obj_t* role = nullptr;
    lv_obj_t* normalDirection = nullptr;
    lv_obj_t* reversedDirection = nullptr;
    lv_obj_t* voltage = nullptr;
    lv_obj_t* current = nullptr;
    lv_obj_t* power = nullptr;
    lv_obj_t* interpretation = nullptr;
    uint8_t physical = 0;
};

lv_obj_t* overlay = nullptr;
lv_obj_t* sourceLabel = nullptr;
lv_obj_t* validationLabel = nullptr;
lv_obj_t* balanceValueLabel = nullptr;
lv_obj_t* balanceHelpLabel = nullptr;
lv_obj_t* cancelButton = nullptr;
lv_obj_t* saveButton = nullptr;
lv_obj_t* saveButtonLabel = nullptr;
lv_timer_t* refreshTimer = nullptr;
SensorRow rows[sensors::mapping::kPhysicalSensorCount];
sensors::mapping::Profile savedProfile{};
sensors::mapping::Profile draftProfile{};
sensor_mode::Mode profileMode = sensor_mode::Mode::Demo;

constexpr lv_coord_t kSensorNameWidth = 83;
constexpr lv_coord_t kDiagnosticStatusWidth = 18;
constexpr lv_coord_t kRoleWidth = 100;
constexpr lv_coord_t kDirectionWidth = 105;
constexpr lv_coord_t kVoltageWidth = 78;
constexpr lv_coord_t kCurrentWidth = 84;
constexpr lv_coord_t kPowerWidth = 114;

bool profilesEqual(const sensors::mapping::Profile& left,
                   const sensors::mapping::Profile& right) {
    return std::memcmp(&left, &right, sizeof(left)) == 0;
}

uint16_t dropdownIndex(sensors::mapping::LogicalRole role) {
    switch (role) {
        case sensors::mapping::LogicalRole::Solar: return 0;
        case sensors::mapping::LogicalRole::Load: return 1;
        case sensors::mapping::LogicalRole::Battery: return 2;
        case sensors::mapping::LogicalRole::Unmapped: return 3;
    }
    return 3;
}

sensors::mapping::LogicalRole dropdownRole(uint16_t selected) {
    switch (selected) {
        case 0: return sensors::mapping::LogicalRole::Solar;
        case 1: return sensors::mapping::LogicalRole::Load;
        case 2: return sensors::mapping::LogicalRole::Battery;
        default: return sensors::mapping::LogicalRole::Unmapped;
    }
}

const char* readingSymbol(const sensors::Reading& reading, bool available) {
    if (!available) return LV_SYMBOL_CLOSE;
    switch (reading.state) {
        case sensors::ReadingState::Valid: return LV_SYMBOL_OK;
        case sensors::ReadingState::OutOfRange: return LV_SYMBOL_WARNING;
        case sensors::ReadingState::NotConfigured:
        case sensors::ReadingState::Waiting:
        case sensors::ReadingState::Invalid:
        case sensors::ReadingState::Stale: return LV_SYMBOL_CLOSE;
    }
    return LV_SYMBOL_CLOSE;
}

bool observed(const sensors::Reading& reading) {
    return reading.state == sensors::ReadingState::Valid ||
           reading.state == sensors::ReadingState::OutOfRange;
}

void formatMeasurement(char* output, size_t outputSize, float value,
                       const char* unit, bool signedValue,
                       uint8_t decimals = 1) {
    if (!std::isfinite(value)) {
        snprintf(output, outputSize, "-- %s", unit);
        return;
    }
    if (decimals == 0) {
        snprintf(output, outputSize, signedValue ? "%+.0f %s" : "%.0f %s",
                 static_cast<double>(value), unit);
    } else {
        snprintf(output, outputSize, signedValue ? "%+.1f %s" : "%.1f %s",
                 static_cast<double>(value), unit);
    }
}

float directionRatio(uint8_t physical) {
    const int8_t saved = sensors::mapping::multiplier(
        savedProfile.physical[physical].currentDirection);
    const int8_t draft = sensors::mapping::multiplier(
        draftProfile.physical[physical].currentDirection);
    return static_cast<float>(draft) / static_cast<float>(saved);
}

bool mappedPower(sensors::mapping::LogicalRole role,
                 const float powers[sensors::mapping::kPhysicalSensorCount],
                 const bool available[sensors::mapping::kPhysicalSensorCount],
                 float& result) {
    for (uint8_t physical = 0;
         physical < sensors::mapping::kPhysicalSensorCount; ++physical) {
        if (draftProfile.physical[physical].role != role) continue;
        if (!available[physical] || !std::isfinite(powers[physical])) return false;
        result = powers[physical];
        return true;
    }
    return false;
}

bool hasMappedRole(sensors::mapping::LogicalRole role) {
    for (const auto& entry : draftProfile.physical) {
        if (entry.role == role) return true;
    }
    return false;
}

const char* interpretationText(sensors::mapping::LogicalRole role,
                               float power, bool available, bool& warning) {
    warning = false;
    if (role == sensors::mapping::LogicalRole::Unmapped) {
        return "None: Not mapped";
    }
    if (!available || !std::isfinite(power)) {
        switch (role) {
            case sensors::mapping::LogicalRole::Solar:
                return "Solar: Waiting for reading";
            case sensors::mapping::LogicalRole::Load:
                return "Load: Waiting for reading";
            case sensors::mapping::LogicalRole::Battery:
                return "Battery: Waiting for reading";
            case sensors::mapping::LogicalRole::Unmapped:
                return "None: Not mapped";
        }
    }
    if (std::fabs(power) < 0.5f) {
        switch (role) {
            case sensors::mapping::LogicalRole::Solar:
                return "Solar: Idle";
            case sensors::mapping::LogicalRole::Load:
                return "Load: Idle";
            case sensors::mapping::LogicalRole::Battery:
                return "Battery: Idle";
            case sensors::mapping::LogicalRole::Unmapped:
                return "None: Not mapped";
        }
    }

    switch (role) {
        case sensors::mapping::LogicalRole::Solar:
            if (power > 0.0f) return "Solar: Producing";
            warning = true;
            return "Solar: Consuming - check polarity";
        case sensors::mapping::LogicalRole::Load:
            if (power > 0.0f) return "Load: Consuming";
            warning = true;
            return "Load: Producing - check polarity";
        case sensors::mapping::LogicalRole::Battery:
            return power > 0.0f
                ? "Battery: Charging" : "Battery: Discharging";
        case sensors::mapping::LogicalRole::Unmapped:
            return "None: Not mapped";
    }
    return "";
}

void updateValidation() {
    const bool valid = sensors::mapping::isValid(draftProfile);
    const bool dirty = !profilesEqual(savedProfile, draftProfile);
    if (!valid) {
        lv_obj_clear_flag(validationLabel, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(
            validationLabel,
            "Assign Solar and Load once. Battery is optional.");
        lv_obj_set_style_text_color(
            validationLabel,
            lv_color_hex(ui_theme::isDark() ? 0xF07A70 : 0xC33B32), 0);
    } else {
        lv_label_set_text(validationLabel, "");
        lv_obj_add_flag(validationLabel, LV_OBJ_FLAG_HIDDEN);
    }
    if (valid && dirty) {
        lv_obj_clear_state(saveButton, LV_STATE_DISABLED);
        lv_obj_set_style_text_color(
            saveButtonLabel, lv_color_white(), 0);
    } else {
        lv_obj_add_state(saveButton, LV_STATE_DISABLED);
        lv_obj_set_style_text_color(
            saveButtonLabel, ui_theme::mutedText(), 0);
    }
}

void updateDiagnostics() {
    float voltages[sensors::mapping::kPhysicalSensorCount]{};
    float currents[sensors::mapping::kPhysicalSensorCount]{};
    float powers[sensors::mapping::kPhysicalSensorCount]{};
    bool available[sensors::mapping::kPhysicalSensorCount]{};
    bool allPowersAboveTen = true;
    bool anyPowerAtLeastThousand = false;

    for (uint8_t physical = 0;
         physical < sensors::mapping::kPhysicalSensorCount; ++physical) {
        sensors::Reading reading{};
        const bool hasReading =
            sensors::getLatestPhysical(physical, reading);
        const float ratio = directionRatio(physical);
        const float voltage = hasReading ? reading.voltage : NAN;
        const float current = hasReading ? reading.current * ratio : NAN;
        const float power = hasReading ? reading.power * ratio : NAN;
        voltages[physical] = voltage;
        currents[physical] = current;
        available[physical] =
            hasReading && observed(reading) && std::isfinite(power);
        powers[physical] = power;
        if (!available[physical] || std::fabs(power) < 10.0f) {
            allPowersAboveTen = false;
        }
        if (available[physical] && std::fabs(power) >= 1000.0f) {
            anyPowerAtLeastThousand = true;
        }

        lv_label_set_text(rows[physical].status,
                          readingSymbol(reading, hasReading));
    }

    const uint8_t powerDecimals =
        allPowersAboveTen || anyPowerAtLeastThousand ? 0 : 1;
    for (uint8_t physical = 0;
         physical < sensors::mapping::kPhysicalSensorCount; ++physical) {
        char text[24];
        formatMeasurement(
            text, sizeof(text), voltages[physical], "V", false);
        lv_label_set_text(rows[physical].voltage, text);
        formatMeasurement(
            text, sizeof(text), currents[physical], "A", true);
        lv_label_set_text(rows[physical].current, text);
        formatMeasurement(
            text, sizeof(text), powers[physical], "W", true, powerDecimals);
        lv_label_set_text(rows[physical].power, text);

        bool interpretationWarning = false;
        lv_label_set_text(
            rows[physical].interpretation,
            interpretationText(
                draftProfile.physical[physical].role,
                powers[physical], available[physical],
                interpretationWarning));
        lv_obj_set_style_text_color(
            rows[physical].interpretation,
            interpretationWarning
                ? lv_color_hex(ui_theme::isDark() ? 0xF07A70 : 0xC33B32)
                : ui_theme::mutedText(),
            0);
    }

    float solar = NAN;
    float load = NAN;
    float battery = NAN;
    char valueText[24];
    char helpText[72];
    const bool validMapping = sensors::mapping::isValid(draftProfile);
    const bool batteryMapped =
        hasMappedRole(sensors::mapping::LogicalRole::Battery);
    const bool haveSolar =
        mappedPower(sensors::mapping::LogicalRole::Solar,
                    powers, available, solar);
    const bool haveLoad =
        mappedPower(sensors::mapping::LogicalRole::Load,
                    powers, available, load);
    const bool haveBattery =
        mappedPower(sensors::mapping::LogicalRole::Battery,
                    powers, available, battery);
    if (validMapping && batteryMapped &&
        haveSolar && haveLoad && haveBattery) {
        const float balance = solar - load - battery;
        const float largestReading =
            std::fmax(std::fabs(solar),
                      std::fmax(std::fabs(load), std::fabs(battery)));
        const float percentage =
            largestReading > 0.05f
                ? std::fabs(balance) * 100.0f / largestReading
                : 0.0f;
        formatMeasurement(
            valueText, sizeof(valueText), balance, "W", true, powerDecimals);
        snprintf(helpText, sizeof(helpText),
                 "Unaccounted power - %.1f%% of max reading",
                 static_cast<double>(percentage));
    } else if (!validMapping) {
        snprintf(valueText, sizeof(valueText), "-- W");
        snprintf(helpText, sizeof(helpText),
                 "Unavailable - map Solar and Load once");
    } else if (!batteryMapped) {
        snprintf(valueText, sizeof(valueText), "-- W");
        snprintf(helpText, sizeof(helpText),
                 "Unavailable - map Battery to calculate");
    } else {
        snprintf(valueText, sizeof(valueText), "-- W");
        snprintf(helpText, sizeof(helpText),
                 "Unavailable - waiting for all three readings");
    }
    lv_label_set_text(balanceValueLabel, valueText);
    lv_label_set_text(balanceHelpLabel, helpText);
}

void selectDirectionControls(SensorRow& row, bool reversed) {
    lv_obj_clear_state(row.normalDirection, LV_STATE_CHECKED);
    lv_obj_clear_state(row.reversedDirection, LV_STATE_CHECKED);
    lv_obj_add_state(
        reversed ? row.reversedDirection : row.normalDirection,
        LV_STATE_CHECKED);
}

void syncDraftControls() {
    for (uint8_t physical = 0;
         physical < sensors::mapping::kPhysicalSensorCount; ++physical) {
        const auto& entry = draftProfile.physical[physical];
        lv_dropdown_set_selected(rows[physical].role,
                                 dropdownIndex(entry.role));
        const bool reversed =
            entry.currentDirection ==
            sensors::mapping::CurrentDirection::Reversed;
        selectDirectionControls(rows[physical], reversed);
    }
}

void roleChangedCb(lv_event_t* event) {
    auto* row = static_cast<SensorRow*>(lv_event_get_user_data(event));
    if (!row) return;
    draftProfile.physical[row->physical].role =
        dropdownRole(lv_dropdown_get_selected(row->role));
    updateDiagnostics();
    updateValidation();
}

void directionChangedCb(lv_event_t* event) {
    auto* row = static_cast<SensorRow*>(lv_event_get_user_data(event));
    if (!row) return;
    const bool reversed =
        lv_event_get_target(event) == row->reversedDirection;
    draftProfile.physical[row->physical].currentDirection =
        reversed ? sensors::mapping::CurrentDirection::Reversed
                 : sensors::mapping::CurrentDirection::Normal;
    selectDirectionControls(*row, reversed);
    updateDiagnostics();
    updateValidation();
}

void closeCb(lv_event_t*) {
    if (overlay) lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);
}

void restartTimerCb(lv_timer_t* timer) {
    lv_timer_del(timer);
    ESP.restart();
}

void saveCb(lv_event_t*) {
    if (!sensors::mapping::isValid(draftProfile) ||
        profilesEqual(savedProfile, draftProfile)) {
        return;
    }
    if (!sensors::mapping::set(profileMode, draftProfile)) {
        lv_obj_clear_flag(validationLabel, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(validationLabel, "Could not save sensor mapping.");
        lv_obj_set_style_text_color(
            validationLabel,
            lv_color_hex(ui_theme::isDark() ? 0xF07A70 : 0xC33B32), 0);
        return;
    }
    lv_label_set_text(saveButtonLabel, "Applying...");
    lv_obj_center(saveButtonLabel);
    lv_obj_add_state(saveButton, LV_STATE_DISABLED);
    lv_obj_add_state(cancelButton, LV_STATE_DISABLED);
    lv_timer_t* restart = lv_timer_create(restartTimerCb, 350, nullptr);
    lv_timer_set_repeat_count(restart, 1);
}

void refreshTimerCb(lv_timer_t*) {
    if (overlay && !lv_obj_has_flag(overlay, LV_OBJ_FLAG_HIDDEN)) {
        updateDiagnostics();
    }
}

lv_obj_t* makeValueLabel(lv_obj_t* parent, lv_coord_t width) {
    lv_obj_t* label = lv_label_create(parent);
    lv_obj_set_width(label, width);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_20, 0);
    return label;
}

void createSensorRow(lv_obj_t* parent, uint8_t physical) {
    SensorRow& row = rows[physical];
    row.physical = physical;

    lv_obj_t* card = lv_obj_create(parent);
    ui_theme::styleCard(card, 4);
    lv_obj_set_size(card, lv_pct(100), 90);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 1, 0);

    lv_obj_t* controls = lv_obj_create(card);
    lv_obj_remove_style_all(controls);
    lv_obj_set_size(controls, lv_pct(100), 38);
    lv_obj_set_flex_flow(controls, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(controls, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(controls, 3, 0);

    lv_obj_t* name = lv_label_create(controls);
    lv_obj_set_width(name, kSensorNameWidth);
    lv_label_set_text(
        name, sensors::mapping::physicalLabel(
                  static_cast<sensors::mapping::PhysicalSensorId>(physical)));
    lv_obj_set_style_text_font(name, &lv_font_montserrat_14, 0);

    row.role = lv_dropdown_create(controls);
    lv_obj_set_size(row.role, kRoleWidth, 38);
    lv_dropdown_set_options(row.role, "Solar\nLoad\nBattery\nNone");
    lv_dropdown_set_symbol(row.role, LV_SYMBOL_DOWN);
    lv_obj_set_style_text_font(row.role, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_pad_top(row.role, 9, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(row.role, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(row.role, roleChangedCb, LV_EVENT_VALUE_CHANGED, &row);

    lv_obj_t* directionGroup = lv_obj_create(controls);
    ui_theme::styleCard(directionGroup, 0);
    lv_obj_set_size(directionGroup, kDirectionWidth, 38);
    lv_obj_set_flex_flow(directionGroup, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(directionGroup, 0, 0);
    lv_obj_clear_flag(directionGroup, LV_OBJ_FLAG_SCROLLABLE);

    row.normalDirection = lv_btn_create(directionGroup);
    row.reversedDirection = lv_btn_create(directionGroup);
    lv_obj_set_flex_grow(row.normalDirection, 1);
    lv_obj_set_flex_grow(row.reversedDirection, 1);
    lv_obj_t* directionButtons[] = {
        row.normalDirection, row.reversedDirection};
    for (lv_obj_t* button : directionButtons) {
        ui_theme::styleSegment(button);
        lv_obj_set_height(button, lv_pct(100));
        lv_obj_set_style_pad_all(button, 0, 0);
        lv_obj_set_style_border_width(button, 0, 0);
        lv_obj_set_style_radius(button, 0, 0);
        lv_obj_add_event_cb(
            button, directionChangedCb, LV_EVENT_CLICKED, &row);
    }
    lv_obj_t* normalLabel = lv_label_create(row.normalDirection);
    lv_label_set_text(normalLabel, "+");
    lv_obj_set_style_text_font(normalLabel, &lv_font_montserrat_20, 0);
    lv_obj_center(normalLabel);
    lv_obj_t* reversedLabel = lv_label_create(row.reversedDirection);
    lv_label_set_text(reversedLabel, "-");
    lv_obj_set_style_text_font(reversedLabel, &lv_font_montserrat_20, 0);
    lv_obj_center(reversedLabel);

    lv_obj_t* diagnostics = lv_obj_create(card);
    lv_obj_remove_style_all(diagnostics);
    lv_obj_set_size(diagnostics, lv_pct(100), 24);
    lv_obj_set_flex_flow(diagnostics, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(diagnostics, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(diagnostics, 0, 0);

    row.status = lv_label_create(diagnostics);
    lv_obj_set_width(row.status, kDiagnosticStatusWidth);
    lv_obj_set_style_text_align(row.status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(row.status, ui_theme::mutedText(), 0);
    row.voltage = makeValueLabel(diagnostics, kVoltageWidth);
    row.current = makeValueLabel(diagnostics, kCurrentWidth);
    row.power = makeValueLabel(diagnostics, kPowerWidth);

    row.interpretation = lv_label_create(card);
    lv_obj_set_size(row.interpretation, lv_pct(100), 16);
    lv_obj_set_style_text_align(
        row.interpretation, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_font(
        row.interpretation, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(
        row.interpretation, ui_theme::mutedText(), 0);
}

void createOverlay() {
    overlay = lv_obj_create(lv_layer_top());
    ui_theme::styleScreen(overlay, 5);
    lv_obj_set_flex_flow(overlay, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(overlay, 3, 0);

    lv_obj_t* header = lv_obj_create(overlay);
    lv_obj_remove_style_all(header);
    lv_obj_set_size(header, lv_pct(100), 52);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* back = lv_btn_create(header);
    lv_obj_remove_style_all(back);
    lv_obj_set_size(back, 38, 40);
    lv_obj_set_ext_click_area(back, 6);
    lv_obj_add_event_cb(back, closeCb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* backLabel = lv_label_create(back);
    lv_label_set_text(backLabel, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(backLabel, ui_theme::accent(), 0);
    lv_obj_center(backLabel);

    lv_obj_t* titleBlock = lv_obj_create(header);
    lv_obj_remove_style_all(titleBlock);
    lv_obj_set_size(titleBlock, 0, 48);
    lv_obj_set_flex_grow(titleBlock, 1);
    lv_obj_set_flex_flow(titleBlock, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(titleBlock, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    lv_obj_t* titleLabel = lv_label_create(titleBlock);
    lv_label_set_text(titleLabel, "Sensor mapping");
    lv_obj_set_style_text_align(titleLabel, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_font(titleLabel, &lv_font_montserrat_18, 0);

    sourceLabel = lv_label_create(titleBlock);
    lv_obj_set_style_text_align(sourceLabel, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_font(sourceLabel, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(sourceLabel, ui_theme::mutedText(), 0);

    for (uint8_t physical = 0;
         physical < sensors::mapping::kPhysicalSensorCount; ++physical) {
        createSensorRow(overlay, physical);
    }

    lv_obj_t* balancePanel = lv_obj_create(overlay);
    lv_obj_remove_style_all(balancePanel);
    lv_obj_set_size(balancePanel, lv_pct(100), 53);
    lv_obj_set_style_pad_left(balancePanel, 5, 0);
    lv_obj_set_style_pad_top(balancePanel, 5, 0);
    lv_obj_set_flex_flow(balancePanel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(balancePanel, 2, 0);

    lv_obj_t* balanceRow = lv_obj_create(balancePanel);
    lv_obj_remove_style_all(balanceRow);
    lv_obj_set_size(balanceRow, 294, 26);
    lv_obj_set_flex_flow(balanceRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(balanceRow, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* balanceTitle = lv_label_create(balanceRow);
    lv_obj_set_width(
        balanceTitle,
        kDiagnosticStatusWidth + kVoltageWidth + kCurrentWidth);
    lv_label_set_text(balanceTitle, "Balance");
    lv_obj_set_style_text_font(balanceTitle, &lv_font_montserrat_18, 0);

    balanceValueLabel = makeValueLabel(balanceRow, kPowerWidth);

    balanceHelpLabel = lv_label_create(balancePanel);
    lv_obj_set_width(balanceHelpLabel, 294);
    lv_obj_set_style_text_align(balanceHelpLabel, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_font(
        balanceHelpLabel, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(
        balanceHelpLabel, ui_theme::mutedText(), 0);

    lv_obj_t* actionSpacer = lv_obj_create(overlay);
    lv_obj_remove_style_all(actionSpacer);
    lv_obj_set_size(actionSpacer, lv_pct(100), 0);
    lv_obj_set_flex_grow(actionSpacer, 1);

    validationLabel = lv_label_create(overlay);
    lv_obj_set_width(validationLabel, lv_pct(100));
    lv_obj_set_style_text_align(validationLabel, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(validationLabel, &lv_font_montserrat_12, 0);

    lv_obj_t* actionRow = lv_obj_create(overlay);
    lv_obj_remove_style_all(actionRow);
    lv_obj_set_size(actionRow, lv_pct(100), 40);
    lv_obj_set_flex_flow(actionRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(actionRow, 5, 0);

    cancelButton = lv_btn_create(actionRow);
    lv_obj_set_flex_grow(cancelButton, 1);
    lv_obj_set_height(cancelButton, 40);
    lv_obj_set_style_bg_color(cancelButton, ui_theme::surfaceAlt(), 0);
    lv_obj_set_style_bg_opa(cancelButton, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(cancelButton, ui_theme::border(), 0);
    lv_obj_set_style_border_width(cancelButton, 1, 0);
    lv_obj_set_style_radius(cancelButton, 6, 0);
    lv_obj_set_style_text_color(cancelButton, ui_theme::text(), 0);
    lv_obj_set_style_opa(cancelButton, LV_OPA_COVER, LV_STATE_DISABLED);
    lv_obj_add_event_cb(cancelButton, closeCb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* cancelLabel = lv_label_create(cancelButton);
    lv_label_set_text(cancelLabel, "Cancel");
    lv_obj_center(cancelLabel);

    saveButton = lv_btn_create(actionRow);
    lv_obj_set_flex_grow(saveButton, 1);
    lv_obj_set_height(saveButton, 40);
    ui_theme::stylePrimaryButton(saveButton);
    lv_obj_add_event_cb(saveButton, saveCb, LV_EVENT_CLICKED, nullptr);
    saveButtonLabel = lv_label_create(saveButton);
    lv_label_set_text(saveButtonLabel, "Save & Reboot");
    lv_obj_center(saveButtonLabel);
    lv_obj_set_style_bg_color(
        saveButton, ui_theme::surfaceAlt(), LV_STATE_DISABLED);
    lv_obj_set_style_bg_opa(
        saveButton, LV_OPA_COVER, LV_STATE_DISABLED);
    lv_obj_set_style_border_color(
        saveButton, ui_theme::border(), LV_STATE_DISABLED);
    lv_obj_set_style_border_width(
        saveButton, 1, LV_STATE_DISABLED);
    lv_obj_set_style_opa(
        saveButton, LV_OPA_COVER, LV_STATE_DISABLED);

    refreshTimer = lv_timer_create(refreshTimerCb, 2000, nullptr);
}

} // namespace

void show() {
    if (!overlay) createOverlay();
    profileMode = sensor_mode::get();
    savedProfile = sensors::mapping::get(profileMode);
    draftProfile = savedProfile;
    lv_label_set_text(sourceLabel, sensor_mode::label());
    lv_label_set_text(saveButtonLabel, "Save & Reboot");
    lv_obj_center(saveButtonLabel);
    lv_obj_clear_state(cancelButton, LV_STATE_DISABLED);
    syncDraftControls();
    updateDiagnostics();
    updateValidation();
    lv_obj_clear_flag(overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(overlay);
}

} // namespace sensor_mapping_overlay
