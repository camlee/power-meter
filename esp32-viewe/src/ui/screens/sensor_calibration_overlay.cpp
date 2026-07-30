#include "sensor_calibration_overlay.h"

#include "sensors/sensor_mode.h"
#include "sensors/sensors.h"
#include "../home_axis.h"
#include "../theme/ui_theme.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <lvgl.h>

namespace sensor_calibration_overlay {
namespace {

constexpr uint32_t kRefreshMs = 500;
constexpr uint32_t kStatusMessageMs = 5000;
constexpr uint16_t kChartPoints = 60;
constexpr float kChartScale = 10.0f;
constexpr lv_coord_t kFocusedKeyboardHeight = 185;
constexpr uint8_t kMaxAxisLabels = 8;

struct RowActions {
    lv_obj_t* title = nullptr;
    lv_obj_t* input = nullptr;
    lv_obj_t* readOnly = nullptr;
    lv_obj_t* unit = nullptr;
    lv_obj_t* spacer = nullptr;
    lv_obj_t* normal = nullptr;
    lv_obj_t* cancel = nullptr;
    lv_obj_t* done = nullptr;
};

lv_obj_t* overlay = nullptr;
lv_obj_t* header = nullptr;
lv_obj_t* subtitleLabel = nullptr;
lv_obj_t* summaryRow = nullptr;
lv_obj_t* oldValueLabel = nullptr;
lv_obj_t* newValueLabel = nullptr;
lv_obj_t* newValueInput = nullptr;
lv_obj_t* gainReadOnlyLabel = nullptr;
lv_obj_t* chartBlock = nullptr;
lv_obj_t* chart = nullptr;
lv_obj_t* axisLabels[kMaxAxisLabels]{};
lv_chart_series_t* oldSeries = nullptr;
lv_chart_series_t* newSeries = nullptr;
lv_obj_t* editorBlock = nullptr;
lv_obj_t* offsetRow = nullptr;
lv_obj_t* gainRow = nullptr;
lv_obj_t* offsetInput = nullptr;
lv_obj_t* gainInput = nullptr;
lv_obj_t* statusLabel = nullptr;
lv_obj_t* actionRow = nullptr;
lv_obj_t* keyboard = nullptr;
lv_obj_t* activeInput = nullptr;
lv_obj_t* activeDoneButton = nullptr;
lv_timer_t* refreshTimer = nullptr;
RowActions offsetActions{};
RowActions gainActions{};

sensors::mapping::PhysicalSensorId activePhysical =
    sensors::mapping::PhysicalSensorId::Sensor1;
sensors::calibration::Measurement activeMeasurement =
    sensors::calibration::Measurement::Voltage;
sensors::calibration::Source activeSource =
    sensors::calibration::Source::Esp32Adc;
sensors::calibration::Value savedValue{};
sensors::calibration::Value stagedValue{};
sensors::calibration::Value keyboardStartValue{};
bool refreshingInputs = false;
bool calculatingGain = false;
bool activeInputValid = false;
bool demoCalibration = false;
uint32_t lastTimestamp = 0;
uint32_t statusMessageUntil = 0;
float observedMinimum = NAN;
float observedMaximum = NAN;
float rangeMinimum = 0.0f;
float rangeMaximum = 1.0f;
float rangeStep = 1.0f;

const char* measurementUnit();

void updateAxisLabels() {
    const int intervals = std::max(
        1, static_cast<int>(lroundf(
               (rangeMaximum - rangeMinimum) / rangeStep)));
    const uint8_t labelCount = static_cast<uint8_t>(
        std::min(intervals + 1, static_cast<int>(kMaxAxisLabels)));
    lv_chart_set_div_line_count(chart, labelCount, 3);
    for (uint8_t index = 0; index < kMaxAxisLabels; ++index) {
        if (!axisLabels[index]) continue;
        if (index >= labelCount) {
            lv_obj_add_flag(axisLabels[index], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_clear_flag(axisLabels[index], LV_OBJ_FLAG_HIDDEN);
        float value = rangeMaximum - rangeStep * index;
        if (std::fabs(value) < rangeStep * 0.001f) value = 0.0f;
        char text[16];
        snprintf(text, sizeof(text), "%.0f %s",
                 static_cast<double>(value), measurementUnit());
        lv_label_set_text(axisLabels[index], text);
    }
}

const char* measurementName() {
    return activeMeasurement == sensors::calibration::Measurement::Voltage
        ? "Voltage" : "Current";
}

const char* measurementUnit() {
    return activeMeasurement == sensors::calibration::Measurement::Voltage
        ? "V" : "A";
}

const char* roleLabel(sensors::mapping::LogicalRole role) {
    switch (role) {
        case sensors::mapping::LogicalRole::Solar: return "Solar";
        case sensors::mapping::LogicalRole::Load: return "Load";
        case sensors::mapping::LogicalRole::Battery: return "Battery";
        case sensors::mapping::LogicalRole::Unmapped: return "None";
    }
    return "None";
}

int8_t currentMultiplier() {
    return sensors::mapping::currentMultiplier(
        sensor_mode::get(), activePhysical);
}

float previewValue(float input) {
    float value = sensors::calibration::apply(input, stagedValue);
    if (activeMeasurement == sensors::calibration::Measurement::Current) {
        value *= static_cast<float>(currentMultiplier());
    }
    return value;
}

float latestDisplayed(const sensors::Reading& reading);

float latestInput(const sensors::Reading& reading) {
    if (demoCalibration) {
        float displayed = latestDisplayed(reading);
        if (activeMeasurement == sensors::calibration::Measurement::Current) {
            displayed *= static_cast<float>(currentMultiplier());
        }
        return displayed / savedValue.gain + savedValue.offsetInputV;
    }
    return activeMeasurement == sensors::calibration::Measurement::Voltage
        ? reading.voltageInputV : reading.currentInputV;
}

float latestDisplayed(const sensors::Reading& reading) {
    return activeMeasurement == sensors::calibration::Measurement::Voltage
        ? reading.voltage : reading.current;
}

void renderStatus(const char* text, bool error) {
    if (!statusLabel) return;
    lv_label_set_text(statusLabel, text ? text : "");
    lv_obj_set_style_text_color(
        statusLabel,
        error
            ? lv_color_hex(ui_theme::isDark() ? 0xF07A70 : 0xC33B32)
            : ui_theme::mutedText(),
        0);
}

bool statusMessageActive() {
    return statusMessageUntil != 0 &&
           static_cast<int32_t>(statusMessageUntil - lv_tick_get()) > 0;
}

void setStatus(const char* text, bool error = false) {
    if (statusMessageActive()) return;
    statusMessageUntil = 0;
    renderStatus(text, error);
}

void showStatusMessage(const char* text, bool error = false) {
    statusMessageUntil = lv_tick_get() + kStatusMessageMs;
    renderStatus(text, error);
}

void styleInput(lv_obj_t* input) {
    lv_textarea_set_one_line(input, true);
    lv_textarea_set_accepted_chars(input, "0123456789.-");
    lv_textarea_set_max_length(input, 12);
    lv_obj_set_size(input, 105, 36);
    lv_obj_set_style_text_font(input, &lv_font_montserrat_16, 0);
    lv_obj_set_style_pad_top(input, 7, 0);
    lv_obj_set_style_pad_bottom(input, 3, 0);
    lv_obj_set_style_border_color(input, ui_theme::border(), 0);
    lv_obj_set_style_border_color(input, ui_theme::accent(), LV_STATE_FOCUSED);
}

void styleRow(lv_obj_t* row) {
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), 40);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(
        row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
        LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 5, 0);
}

lv_obj_t* makeSecondaryButton(
    lv_obj_t* parent, const char* text, lv_event_cb_t callback,
    lv_coord_t width = 54) {
    lv_obj_t* button = lv_btn_create(parent);
    lv_obj_set_size(button, width, 36);
    lv_obj_set_style_bg_color(button, ui_theme::surfaceAlt(), 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(button, ui_theme::border(), 0);
    lv_obj_set_style_border_width(button, 1, 0);
    lv_obj_set_style_radius(button, 6, 0);
    lv_obj_set_style_text_color(button, ui_theme::text(), 0);
    if (callback) {
        lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, nullptr);
    }
    lv_obj_t* label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    return button;
}

void configureRowActions(RowActions& actions, bool focused) {
    lv_obj_set_width(actions.title, focused ? 45 : 78);
    lv_obj_set_width(actions.input, focused ? 82 : 105);
    if (actions.readOnly) {
        lv_obj_set_width(actions.readOnly, focused ? 82 : 105);
    }
    lv_obj_set_width(actions.unit, focused ? 35 : 38);
    if (focused) {
        lv_obj_clear_flag(actions.spacer, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(actions.normal, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(actions.cancel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(actions.done, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(actions.spacer, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(actions.normal, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(actions.cancel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(actions.done, LV_OBJ_FLAG_HIDDEN);
    }
}

void setDoneEnabled(bool enabled) {
    if (!activeDoneButton) return;
    if (enabled) {
        lv_obj_clear_state(activeDoneButton, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(activeDoneButton, LV_STATE_DISABLED);
    }
}

void refreshInputs() {
    if (!offsetInput || !gainInput) return;
    refreshingInputs = true;
    char text[24];
    snprintf(text, sizeof(text), "%.4f",
             static_cast<double>(stagedValue.offsetInputV));
    lv_textarea_set_text(offsetInput, text);
    snprintf(text, sizeof(text), "%.3f",
             static_cast<double>(1000.0f / stagedValue.gain));
    lv_textarea_set_text(gainInput, text);
    if (gainReadOnlyLabel) lv_label_set_text(gainReadOnlyLabel, text);
    refreshingInputs = false;
}

void updateChartRange(float oldValue, float newValue) {
    if (!std::isfinite(oldValue) || !std::isfinite(newValue)) return;
    const float low = std::min(oldValue, newValue);
    const float high = std::max(oldValue, newValue);
    const float margin = std::max(
        activeMeasurement == sensors::calibration::Measurement::Voltage
            ? 0.5f : 1.0f,
        (high - low) * 0.2f);
    observedMinimum = std::isfinite(observedMinimum)
        ? std::min(observedMinimum, low - margin) : low - margin;
    observedMaximum = std::isfinite(observedMaximum)
        ? std::max(observedMaximum, high + margin) : high + margin;
    const home_axis::Scale scale =
        home_axis::scale(observedMinimum, observedMaximum);
    rangeMinimum = scale.minimum;
    rangeMaximum = scale.maximum;
    rangeStep = scale.step;
    lv_chart_set_range(
        chart, LV_CHART_AXIS_PRIMARY_Y,
        static_cast<lv_coord_t>(std::floor(rangeMinimum * kChartScale)),
        static_cast<lv_coord_t>(std::ceil(rangeMaximum * kChartScale)));
    updateAxisLabels();
}

void refreshLatest(bool appendPoint) {
    if (!overlay) return;
    sensors::Reading reading{};
    if (!sensors::getLatestPhysical(
            static_cast<uint8_t>(activePhysical), reading)) {
        lv_label_set_text(oldValueLabel, "--");
        lv_label_set_text(newValueLabel, "--");
        setStatus("Waiting for sensor reading");
        return;
    }
    const float input = latestInput(reading);
    const float oldValue = latestDisplayed(reading);
    const float newValue = previewValue(input);
    char text[24];
    if (std::isfinite(oldValue)) {
        snprintf(text, sizeof(text), "%.1f %s",
                 static_cast<double>(oldValue), measurementUnit());
    } else {
        snprintf(text, sizeof(text), "-- %s", measurementUnit());
    }
    lv_label_set_text(oldValueLabel, text);
    if (!calculatingGain) {
        if (std::isfinite(newValue)) {
            snprintf(text, sizeof(text), "%.1f %s",
                     static_cast<double>(newValue), measurementUnit());
        } else {
            snprintf(text, sizeof(text), "-- %s", measurementUnit());
        }
        lv_label_set_text(newValueLabel, text);
    }

    if (!std::isfinite(input) || !std::isfinite(oldValue) ||
        !std::isfinite(newValue)) {
        setStatus("Current reading cannot be calibrated", true);
        return;
    }
    if (reading.state == sensors::ReadingState::OutOfRange) {
        setStatus("Reading is outside the operating range", true);
    } else if (reading.state == sensors::ReadingState::Valid) {
        setStatus("");
    } else {
        setStatus("Reading is unavailable", true);
    }

    if (appendPoint && reading.timestamp_ms != lastTimestamp) {
        updateChartRange(oldValue, newValue);
        lv_chart_set_next_value(
            chart, oldSeries,
            static_cast<lv_coord_t>(lroundf(oldValue * kChartScale)));
        lv_chart_set_next_value(
            chart, newSeries,
            static_cast<lv_coord_t>(lroundf(newValue * kChartScale)));
        lastTimestamp = reading.timestamp_ms;
    }
}

void restoreNormalLayout() {
    if (!overlay) return;
    lv_obj_set_height(editorBlock, 82);
    configureRowActions(offsetActions, false);
    configureRowActions(gainActions, false);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(summaryRow, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(chartBlock, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(offsetRow, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(gainRow, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(newValueLabel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(newValueInput, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(gainInput, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(gainReadOnlyLabel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(statusLabel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(actionRow, LV_OBJ_FLAG_HIDDEN);
    activeDoneButton = nullptr;
    activeInputValid = false;
}

bool updateCalculatedGain();
void closeCalculation(bool accept);

void closeKeyboard(bool accept) {
    if (!keyboard) return;
    if (!accept) {
        stagedValue = keyboardStartValue;
        refreshInputs();
    }
    lv_obj_del(keyboard);
    keyboard = nullptr;
    activeInput = nullptr;
    calculatingGain = false;
    restoreNormalLayout();
    refreshLatest(false);
}

void keyboardCb(lv_event_t* event) {
    const lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
        if (calculatingGain) {
            if (code == LV_EVENT_CANCEL ||
                updateCalculatedGain()) {
                closeCalculation(code == LV_EVENT_READY);
            }
        } else if (code == LV_EVENT_CANCEL || activeInputValid) {
            closeKeyboard(code == LV_EVENT_READY);
        }
    }
}

void showKeyboard(lv_obj_t* input) {
    if (keyboard || !input) return;
    keyboardStartValue = stagedValue;
    activeInput = input;
    activeInputValid = false;

    lv_obj_add_flag(header, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(actionRow, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(statusLabel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_height(editorBlock, 40);
    if (input == offsetInput) {
        lv_obj_clear_flag(offsetRow, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(gainRow, LV_OBJ_FLAG_HIDDEN);
        configureRowActions(offsetActions, true);
        activeDoneButton = offsetActions.done;
    } else {
        lv_obj_add_flag(offsetRow, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(gainRow, LV_OBJ_FLAG_HIDDEN);
        configureRowActions(gainActions, true);
        activeDoneButton = gainActions.done;
    }
    refreshingInputs = true;
    lv_textarea_set_text(input, "");
    refreshingInputs = false;
    setDoneEnabled(false);

    keyboard = lv_keyboard_create(overlay);
    lv_obj_set_size(
        keyboard, lv_pct(100), kFocusedKeyboardHeight);
    lv_keyboard_set_mode(keyboard, LV_KEYBOARD_MODE_NUMBER);
    lv_keyboard_set_textarea(keyboard, input);
    lv_obj_add_event_cb(keyboard, keyboardCb, LV_EVENT_READY, nullptr);
    lv_obj_add_event_cb(keyboard, keyboardCb, LV_EVENT_CANCEL, nullptr);
}

void inputFocusCb(lv_event_t* event) {
    showKeyboard(static_cast<lv_obj_t*>(lv_event_get_target(event)));
}

void inputChangedCb(lv_event_t* event) {
    if (refreshingInputs) return;
    lv_obj_t* target = static_cast<lv_obj_t*>(lv_event_get_target(event));
    char* end = nullptr;
    const char* valueText = lv_textarea_get_text(target);
    const float value = strtof(valueText, &end);
    if (!end || end == valueText || *end != '\0' || !std::isfinite(value)) {
        activeInputValid = false;
        setDoneEnabled(false);
        return;
    }
    sensors::calibration::Value candidate = stagedValue;
    if (target == offsetInput) {
        candidate.offsetInputV = value;
    } else if (target == gainInput && value > 0.0f) {
        candidate.gain = 1000.0f / value;
    } else {
        activeInputValid = false;
        setDoneEnabled(false);
        return;
    }
    activeInputValid =
        sensors::calibration::isValid(activeMeasurement, candidate);
    setDoneEnabled(activeInputValid);
    if (!activeInputValid) {
        return;
    }
    stagedValue = candidate;
    refreshLatest(false);
}

void referenceChangedCb(lv_event_t*) {
    if (refreshingInputs || !calculatingGain) return;
    activeInputValid = updateCalculatedGain();
    setDoneEnabled(activeInputValid);
}

void closeOverlay() {
    if (refreshTimer) {
        lv_timer_del(refreshTimer);
        refreshTimer = nullptr;
    }
    if (overlay) {
        lv_obj_del(overlay);
        overlay = nullptr;
    }
    keyboard = nullptr;
    activeInput = nullptr;
    activeDoneButton = nullptr;
    calculatingGain = false;
    activeInputValid = false;
}

void closeCb(lv_event_t*) { closeOverlay(); }

void zeroCb(lv_event_t*) {
    sensors::Reading reading{};
    if (!sensors::getLatestPhysical(
            static_cast<uint8_t>(activePhysical), reading) ||
        !std::isfinite(latestInput(reading))) {
        showStatusMessage("No finite input is available for zero", true);
        return;
    }
    sensors::calibration::Value candidate = stagedValue;
    candidate.offsetInputV = latestInput(reading);
    if (!sensors::calibration::isValid(activeMeasurement, candidate)) {
        showStatusMessage("Zero is outside the allowed range", true);
        return;
    }
    stagedValue = candidate;
    refreshInputs();
    refreshLatest(false);
}

bool updateCalculatedGain() {
    char* end = nullptr;
    const char* referenceText = lv_textarea_get_text(newValueInput);
    const float reference = strtof(referenceText, &end);
    const float maximum =
        activeMeasurement == sensors::calibration::Measurement::Voltage
            ? sensors::calibration::kVoltageMaxV
            : sensors::calibration::kCurrentMaxA;
    sensors::Reading reading{};
    if (!end || end == referenceText || *end != '\0' ||
        !std::isfinite(reference) || reference <= 0.0f ||
        reference > maximum ||
        !sensors::getLatestPhysical(
            static_cast<uint8_t>(activePhysical), reading)) {
        return false;
    }
    const float denominator =
        (latestInput(reading) - stagedValue.offsetInputV) *
        (activeMeasurement == sensors::calibration::Measurement::Current
             ? static_cast<float>(currentMultiplier())
             : 1.0f);
    if (!std::isfinite(denominator) || std::fabs(denominator) < 0.005f) {
        return false;
    }
    sensors::calibration::Value candidate = stagedValue;
    candidate.gain = reference / denominator;
    if (!sensors::calibration::isValid(activeMeasurement, candidate)) {
        return false;
    }
    stagedValue = candidate;
    refreshInputs();
    refreshLatest(false);
    return true;
}

void closeCalculation(bool accept) {
    if (!calculatingGain) return;
    if (!accept) stagedValue = keyboardStartValue;
    calculatingGain = false;
    if (keyboard) {
        lv_obj_del(keyboard);
        keyboard = nullptr;
    }
    activeInput = nullptr;
    restoreNormalLayout();
    refreshInputs();
    refreshLatest(false);
    setStatus("");
}

void calculateCb(lv_event_t*) {
    if (keyboard || calculatingGain) return;
    keyboardStartValue = stagedValue;
    calculatingGain = true;

    refreshingInputs = true;
    lv_textarea_set_text(newValueInput, "");
    refreshingInputs = false;
    lv_obj_add_flag(header, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(offsetRow, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(actionRow, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(statusLabel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(newValueLabel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(newValueInput, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(gainInput, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(gainReadOnlyLabel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_height(editorBlock, 40);
    configureRowActions(gainActions, true);
    activeDoneButton = gainActions.done;
    activeInputValid = false;
    setDoneEnabled(false);

    keyboard = lv_keyboard_create(overlay);
    lv_obj_set_size(
        keyboard, lv_pct(100), kFocusedKeyboardHeight);
    lv_keyboard_set_mode(keyboard, LV_KEYBOARD_MODE_NUMBER);
    lv_keyboard_set_textarea(keyboard, newValueInput);
    lv_obj_add_event_cb(keyboard, keyboardCb, LV_EVENT_READY, nullptr);
    lv_obj_add_event_cb(keyboard, keyboardCb, LV_EVENT_CANCEL, nullptr);
    activeInput = newValueInput;
}

void resetCb(lv_event_t*) {
    stagedValue = sensors::calibration::defaults(
        activeSource, static_cast<uint8_t>(activePhysical),
        activeMeasurement);
    refreshInputs();
    refreshLatest(false);
    showStatusMessage("Factory calibration staged");
}

void saveCb(lv_event_t*) {
    if (!sensors::calibration::isValid(activeMeasurement, stagedValue)) {
        showStatusMessage(
            "Calibration values are outside the allowed range", true);
        return;
    }
    if (demoCalibration) {
        closeOverlay();
        return;
    }
    if (!sensors::calibration::set(
            activeSource, static_cast<uint8_t>(activePhysical),
            activeMeasurement, stagedValue)) {
        showStatusMessage("Could not save calibration", true);
        return;
    }
    savedValue = stagedValue;
    closeOverlay();
}

void refreshTimerCb(lv_timer_t*) { refreshLatest(true); }

void editCancelCb(lv_event_t*) {
    if (calculatingGain) {
        closeCalculation(false);
    } else {
        closeKeyboard(false);
    }
}

void editDoneCb(lv_event_t*) {
    if (!activeInputValid) return;
    if (calculatingGain) {
        if (updateCalculatedGain()) closeCalculation(true);
    } else {
        closeKeyboard(true);
    }
}

lv_obj_t* createFieldRow(
    lv_obj_t* parent, const char* title, lv_obj_t** inputOut,
    const char* unit, const char* action, lv_event_cb_t actionCb,
    RowActions* actions, lv_obj_t** readOnlyOut = nullptr) {
    lv_obj_t* row = lv_obj_create(parent);
    styleRow(row);
    lv_obj_t* label = lv_label_create(row);
    lv_obj_set_width(label, 78);
    lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
    lv_label_set_text(label, title);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
    actions->title = label;

    lv_obj_t* input = lv_textarea_create(row);
    styleInput(input);
    lv_obj_add_event_cb(input, inputFocusCb, LV_EVENT_FOCUSED, nullptr);
    lv_obj_add_event_cb(
        input, inputChangedCb, LV_EVENT_VALUE_CHANGED, nullptr);
    *inputOut = input;
    actions->input = input;

    if (readOnlyOut) {
        lv_obj_t* readOnly = lv_label_create(row);
        lv_obj_set_width(readOnly, 105);
        lv_label_set_long_mode(readOnly, LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_font(
            readOnly, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_align(
            readOnly, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_add_flag(readOnly, LV_OBJ_FLAG_HIDDEN);
        *readOnlyOut = readOnly;
        actions->readOnly = readOnly;
    }

    lv_obj_t* unitLabel = lv_label_create(row);
    lv_obj_set_width(unitLabel, 38);
    lv_label_set_long_mode(unitLabel, LV_LABEL_LONG_CLIP);
    lv_label_set_text(unitLabel, unit);
    lv_obj_set_style_text_font(unitLabel, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(unitLabel, ui_theme::mutedText(), 0);
    actions->unit = unitLabel;

    if (action && action[0]) {
        actions->normal =
            makeSecondaryButton(row, action, actionCb, 58);
    }
    actions->spacer = lv_obj_create(row);
    lv_obj_remove_style_all(actions->spacer);
    lv_obj_set_size(actions->spacer, 0, 1);
    lv_obj_set_flex_grow(actions->spacer, 1);
    lv_obj_add_flag(actions->spacer, LV_OBJ_FLAG_HIDDEN);
    actions->cancel =
        makeSecondaryButton(row, "Cancel", editCancelCb, 60);
    actions->done = lv_btn_create(row);
    lv_obj_set_size(actions->done, 50, 36);
    ui_theme::stylePrimaryButton(actions->done);
    lv_obj_add_event_cb(
        actions->done, editDoneCb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* doneLabel = lv_label_create(actions->done);
    lv_label_set_text(doneLabel, "Done");
    lv_obj_center(doneLabel);
    lv_obj_add_flag(actions->cancel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(actions->done, LV_OBJ_FLAG_HIDDEN);
    return row;
}

void createOverlay() {
    overlay = lv_obj_create(lv_layer_top());
    ui_theme::styleScreen(overlay, 5);
    lv_obj_set_flex_flow(overlay, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(overlay, 4, 0);

    header = lv_obj_create(overlay);
    lv_obj_remove_style_all(header);
    lv_obj_set_size(header, lv_pct(100), 52);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(
        header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
        LV_FLEX_ALIGN_CENTER);

    lv_obj_t* back = lv_btn_create(header);
    lv_obj_remove_style_all(back);
    lv_obj_set_size(back, 38, 42);
    lv_obj_set_ext_click_area(back, 6);
    lv_obj_add_event_cb(back, closeCb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* backLabel = lv_label_create(back);
    lv_label_set_text(backLabel, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(backLabel, ui_theme::accent(), 0);
    lv_obj_center(backLabel);

    lv_obj_t* titleBlock = lv_obj_create(header);
    lv_obj_remove_style_all(titleBlock);
    lv_obj_set_size(titleBlock, 0, 50);
    lv_obj_set_flex_grow(titleBlock, 1);
    lv_obj_set_flex_flow(titleBlock, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(
        titleBlock, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START,
        LV_FLEX_ALIGN_START);
    lv_obj_t* title = lv_label_create(titleBlock);
    lv_label_set_text(title, "Calibration");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_18, 0);
    subtitleLabel = lv_label_create(titleBlock);
    lv_obj_set_width(subtitleLabel, lv_pct(100));
    lv_label_set_long_mode(subtitleLabel, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(
        subtitleLabel, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(subtitleLabel, ui_theme::mutedText(), 0);

    summaryRow = lv_obj_create(overlay);
    ui_theme::styleCard(summaryRow, 4);
    lv_obj_set_size(summaryRow, lv_pct(100), 42);
    lv_obj_set_flex_flow(summaryRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(
        summaryRow, LV_FLEX_ALIGN_SPACE_AROUND, LV_FLEX_ALIGN_CENTER,
        LV_FLEX_ALIGN_CENTER);
    lv_obj_t* oldLabel = lv_label_create(summaryRow);
    lv_label_set_text(oldLabel, "Old");
    lv_obj_set_style_text_color(
        oldLabel, lv_palette_main(LV_PALETTE_BLUE), 0);
    oldValueLabel = lv_label_create(summaryRow);
    lv_obj_set_width(oldValueLabel, 70);
    lv_obj_set_style_text_align(oldValueLabel, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_font(oldValueLabel, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(
        oldValueLabel, lv_palette_main(LV_PALETTE_BLUE), 0);
    lv_obj_t* newLabel = lv_label_create(summaryRow);
    lv_label_set_text(newLabel, "New");
    lv_obj_set_style_text_color(
        newLabel, lv_palette_main(LV_PALETTE_ORANGE), 0);
    newValueLabel = lv_label_create(summaryRow);
    lv_obj_set_width(newValueLabel, 70);
    lv_obj_set_style_text_align(newValueLabel, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_font(newValueLabel, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(
        newValueLabel, lv_palette_main(LV_PALETTE_ORANGE), 0);
    newValueInput = lv_textarea_create(summaryRow);
    styleInput(newValueInput);
    lv_obj_set_size(newValueInput, 78, 34);
    lv_obj_set_style_text_color(
        newValueInput, lv_palette_main(LV_PALETTE_ORANGE), 0);
    lv_obj_set_style_border_color(
        newValueInput, lv_palette_main(LV_PALETTE_ORANGE), 0);
    lv_obj_add_event_cb(
        newValueInput, referenceChangedCb, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_flag(newValueInput, LV_OBJ_FLAG_HIDDEN);

    chartBlock = lv_obj_create(overlay);
    ui_theme::styleCard(chartBlock, 4);
    lv_obj_set_size(chartBlock, lv_pct(100), 0);
    lv_obj_set_flex_grow(chartBlock, 1);
    lv_obj_set_flex_flow(chartBlock, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(chartBlock, 1, 0);

    lv_obj_t* chartRow = lv_obj_create(chartBlock);
    lv_obj_remove_style_all(chartRow);
    lv_obj_set_size(chartRow, lv_pct(100), 0);
    lv_obj_set_flex_grow(chartRow, 1);
    lv_obj_set_flex_flow(chartRow, LV_FLEX_FLOW_ROW);

    lv_obj_t* yAxis = lv_obj_create(chartRow);
    lv_obj_remove_style_all(yAxis);
    lv_obj_set_size(yAxis, 45, lv_pct(100));
    lv_obj_set_flex_flow(yAxis, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(
        yAxis, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_END,
        LV_FLEX_ALIGN_END);
    for (lv_obj_t*& label : axisLabels) {
        label = lv_label_create(yAxis);
        lv_obj_set_width(label, 44);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_10, 0);
        lv_obj_set_style_text_color(label, ui_theme::mutedText(), 0);
    }

    chart = lv_chart_create(chartRow);
    lv_obj_set_size(chart, 0, lv_pct(100));
    lv_obj_set_flex_grow(chart, 1);
    lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(chart, kChartPoints);
    lv_chart_set_update_mode(chart, LV_CHART_UPDATE_MODE_SHIFT);
    lv_chart_set_div_line_count(chart, 3, 3);
    lv_obj_set_style_bg_opa(chart, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(chart, 0, LV_PART_MAIN);
    lv_obj_set_style_line_color(chart, ui_theme::border(), LV_PART_MAIN);
    lv_obj_set_style_line_width(chart, 1, LV_PART_MAIN);
    lv_obj_set_style_size(chart, 0, LV_PART_INDICATOR);
    oldSeries = lv_chart_add_series(
        chart, lv_palette_main(LV_PALETTE_BLUE),
        LV_CHART_AXIS_PRIMARY_Y);
    newSeries = lv_chart_add_series(
        chart, lv_palette_main(LV_PALETTE_ORANGE),
        LV_CHART_AXIS_PRIMARY_Y);
    lv_chart_set_all_value(chart, oldSeries, LV_CHART_POINT_NONE);
    lv_chart_set_all_value(chart, newSeries, LV_CHART_POINT_NONE);

    lv_obj_t* xAxis = lv_obj_create(chartBlock);
    lv_obj_remove_style_all(xAxis);
    lv_obj_set_size(xAxis, lv_pct(100), 15);
    lv_obj_set_style_pad_left(xAxis, 45, 0);
    lv_obj_set_style_pad_right(xAxis, 2, 0);
    lv_obj_set_flex_flow(xAxis, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(
        xAxis, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
        LV_FLEX_ALIGN_CENTER);
    const char* timeLabels[] = {"-30s", "-15s", "now"};
    for (const char* text : timeLabels) {
        lv_obj_t* label = lv_label_create(xAxis);
        lv_label_set_text(label, text);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_10, 0);
        lv_obj_set_style_text_color(label, ui_theme::mutedText(), 0);
    }

    editorBlock = lv_obj_create(overlay);
    lv_obj_remove_style_all(editorBlock);
    lv_obj_set_size(editorBlock, lv_pct(100), 82);
    lv_obj_set_flex_flow(editorBlock, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(editorBlock, 2, 0);
    offsetRow = createFieldRow(
        editorBlock, "Offset", &offsetInput, "V", "Zero", zeroCb,
        &offsetActions);
    gainRow = createFieldRow(
        editorBlock, "Gain", &gainInput,
        activeMeasurement == sensors::calibration::Measurement::Voltage
            ? "mV/V" : "mV/A",
        "Calc", calculateCb, &gainActions, &gainReadOnlyLabel);

    statusLabel = lv_label_create(overlay);
    lv_obj_set_size(statusLabel, lv_pct(100), 18);
    lv_label_set_long_mode(statusLabel, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(statusLabel, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(statusLabel, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(statusLabel, ui_theme::mutedText(), 0);

    actionRow = lv_obj_create(overlay);
    lv_obj_remove_style_all(actionRow);
    lv_obj_set_size(actionRow, lv_pct(100), 40);
    lv_obj_set_flex_flow(actionRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(actionRow, 5, 0);
    lv_obj_t* cancel = makeSecondaryButton(
        actionRow, "Cancel", closeCb, 0);
    lv_obj_set_flex_grow(cancel, 1);
    lv_obj_t* reset = makeSecondaryButton(
        actionRow, "Reset", resetCb, 0);
    lv_obj_set_flex_grow(reset, 1);
    lv_obj_t* save = lv_btn_create(actionRow);
    lv_obj_set_height(save, 36);
    lv_obj_set_flex_grow(save, 1);
    ui_theme::stylePrimaryButton(save);
    lv_obj_add_event_cb(save, saveCb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* saveLabel = lv_label_create(save);
    lv_label_set_text(saveLabel, "Save");
    lv_obj_center(saveLabel);
}

} // namespace

void show(
    sensors::mapping::PhysicalSensorId physical,
    sensors::calibration::Measurement measurement) {
    const sensor_mode::Mode mode = sensor_mode::get();
    if (!sensor_mode::usesCalibration(mode) &&
        mode != sensor_mode::Mode::Demo) {
        return;
    }
    closeOverlay();
    demoCalibration = mode == sensor_mode::Mode::Demo;
    activePhysical = physical;
    activeMeasurement = measurement;
    activeSource =
        mode == sensor_mode::Mode::Ads1115
            ? sensors::calibration::Source::Ads1115
            : sensors::calibration::Source::Esp32Adc;
    savedValue = demoCalibration
        ? sensors::calibration::defaults(
              activeSource, static_cast<uint8_t>(activePhysical),
              activeMeasurement)
        : sensors::calibration::get(
              activeSource, static_cast<uint8_t>(activePhysical),
              activeMeasurement);
    stagedValue = savedValue;
    lastTimestamp = 0;
    statusMessageUntil = 0;
    observedMinimum = NAN;
    observedMaximum = NAN;
    rangeMinimum = 0.0f;
    rangeMaximum = 1.0f;
    rangeStep = 1.0f;
    createOverlay();

    const sensors::mapping::Profile profile =
        sensors::mapping::get(mode);
    const auto role =
        profile.physical[static_cast<uint8_t>(activePhysical)].role;
    char subtitle[64];
    snprintf(
        subtitle, sizeof(subtitle), "%s (%s) - %s",
        sensors::mapping::physicalLabel(activePhysical), roleLabel(role),
        measurementName());
    lv_label_set_text(subtitleLabel, subtitle);
    lv_label_set_text(
        lv_obj_get_child(gainRow, 2),
        activeMeasurement == sensors::calibration::Measurement::Voltage
            ? "mV/V" : "mV/A");
    updateAxisLabels();
    refreshInputs();
    refreshLatest(true);
    refreshTimer = lv_timer_create(refreshTimerCb, kRefreshMs, nullptr);
}

} // namespace sensor_calibration_overlay
