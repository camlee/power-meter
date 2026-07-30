#include "sensors_screen.h"
#include "sensor_calibration_overlay.h"
#include "../../sensors/sensors.h"
#include "../../sensors/sensor_calibration.h"
#include "../../sensors/sensor_mapping.h"
#include "../../sensors/sensor_mode.h"
#include "../../device/hardware_profile.h"
#include "../../memory/heap_policy.h"
#include "../theme/ui_theme.h"
#include "../navigation/tabview_utils.h"
#include <esp_heap_caps.h>
#include <cstdio>
#include <cmath>
#include <algorithm>
#include <cstdlib>
#include <limits>

// Assumptions / lv_conf.h requirements (please confirm on your build):
//  - LVGL 8.3.x API (lv_chart_set_axis_tick, LV_EVENT_DRAW_PART_BEGIN, LV_PART_TICKS)
//  - LV_FONT_MONTSERRAT_14 and LV_FONT_MONTSERRAT_20 enabled for the KPI/label fonts used below.
//    If only the default 14px font is enabled, swap &lv_font_montserrat_20 for
//    &lv_font_montserrat_14 (or your preferred large font) in createKpiItem().

namespace sensors_screen {
namespace {

constexpr uint32_t kUpdateIntervalMs = 500;          // match sensors::kSampleIntervalMs
constexpr size_t kChartPoints = 60;                  // visible window; must be <= sensors::kHistorySize
constexpr uint32_t kWindowMs = kChartPoints * kUpdateIntervalMs; // 30s visible window

constexpr uint32_t kKpiUpdateIntervalMs = 2000;       // how often the numeric readouts refresh
constexpr uint32_t kKpiAverageWindowMs = 1000;        // readouts show the trailing 1s average

constexpr uint32_t kGridIntervalMs = 10000;           // vertical grid line spacing
constexpr size_t kGridPointsPerDiv = kGridIntervalMs / kUpdateIntervalMs; // points per grid column
constexpr size_t kGridDivisions = kChartPoints / kGridPointsPerDiv;       // e.g. 30s / 10s = 3

constexpr float kDutyShowThreshold = 0.80f;           // duty reveal trips below this, then stays shown

// Chart values are stored as integers (lv_coord_t). To keep one decimal place
// of resolution on V/I we store value*kXxxAxisScale and divide back down
// when drawing tick labels (see axisTickLabelCb).
static const float kVoltageAxisScale = 10.0f;
static const float kCurrentAxisScale = 10.0f;

lv_coord_t chartCoordinate(float value, float scale) {
    const double scaled = static_cast<double>(value) * scale;
    if (!std::isfinite(scaled) ||
        scaled < std::numeric_limits<lv_coord_t>::lowest() ||
        scaled >= std::numeric_limits<lv_coord_t>::max()) return LV_CHART_POINT_NONE;
    return static_cast<lv_coord_t>(lround(scaled));
}

lv_coord_t axisCoordinate(float value, float scale) {
    const double scaled = static_cast<double>(value) * scale;
    return static_cast<lv_coord_t>(std::max<double>(
        std::numeric_limits<lv_coord_t>::lowest(),
        std::min<double>(std::numeric_limits<lv_coord_t>::max() - 1, scaled)));
}

// Sane default axis windows; these expand automatically if real data falls
// outside them (see computeDynamicRange), and use a bit of margin so the
// trace doesn't hug the top/bottom edge.
constexpr float kVoltageDefaultMin = 12.0f;
constexpr float kVoltageDefaultMax = 14.0f;
constexpr float kCurrentDefaultMin = 0.0f;
constexpr float kCurrentDefaultMax = 5.0f;

// ---------------------------------------------------------------------------
// Dynamic "nice" axis ranging
// ---------------------------------------------------------------------------

// Rounds a raw step size up to a "nice" 1/2/5 * 10^n value so axis bounds
// land on round numbers instead of e.g. 12.37.
float niceStep(float rawStep) {
    if (rawStep <= 0.0f) return 1.0f;
    float exponent = floorf(log10f(rawStep));
    float base = rawStep / powf(10.0f, exponent);
    float niceBase;
    if (base <= 1.0f) niceBase = 1.0f;
    else if (base <= 2.0f) niceBase = 2.0f;
    else if (base <= 5.0f) niceBase = 5.0f;
    else niceBase = 10.0f;
    return niceBase * powf(10.0f, exponent);
}

// Tracks the currently-displayed range for one axis so we can apply
// hysteresis and avoid the axis jittering every 500ms update.
struct AxisRangeState {
    float curMin = 0.0f;
    float curMax = 1.0f;
    bool initialized = false;
};

struct SensorChartFrame {
    AxisRangeState* range = nullptr;
    float scale = 1.0f;
    lv_obj_t* axisLabels[5] = {};
};

struct RawCaptureChartFrame {
    float minimum = 0.0f;
    float maximum = 1.0f;
    float scale = 1.0f;
    const char* unit = "";
    lv_obj_t* axisLabels[4] = {};
    uint16_t pointCount = 0;
    uint16_t boundaries[sensors::kAdcCaptureWindowCount + 1] = {};
    uint8_t boundaryCount = 0;
};

// Computes a new nice range that (a) always covers at least [defaultMin,
// defaultMax], and (b) expands to cover the actual data + margin when the
// data goes outside the default window (e.g. current spiking to 20A).
// Only moves the previously-displayed range if the data no longer fits, or
// the ideal range has shrunk substantially - this keeps the axis stable
// during normal operation instead of constantly re-snapping.
void computeDynamicRange(float dataMin, float dataMax, float defaultMin, float defaultMax,
                          float margin, AxisRangeState& state, float& outMin, float& outMax) {
    float wantMin = std::min(defaultMin, dataMin - margin);
    float wantMax = std::max(defaultMax, dataMax + margin);
    if (wantMax <= wantMin) wantMax = wantMin + margin;

    float step = niceStep((wantMax - wantMin) / 4.0f);
    float niceMin = floorf(wantMin / step) * step;
    float niceMax = ceilf(wantMax / step) * step;

    if (!state.initialized) {
        state.curMin = niceMin;
        state.curMax = niceMax;
        state.initialized = true;
    } else {
        bool dataOutsideCurrent = (dataMin - margin) < state.curMin || (dataMax + margin) > state.curMax;
        bool currentTooWide = (niceMax - niceMin) < (state.curMax - state.curMin) * 0.6f;
        if (dataOutsideCurrent || currentTooWide) {
            state.curMin = niceMin;
            state.curMax = niceMax;
        }
    }
    outMin = state.curMin;
    outMax = state.curMax;
}

// ---------------------------------------------------------------------------
// Per-tab state
// ---------------------------------------------------------------------------

struct SensorTab {
    lv_obj_t* contentRoot = nullptr;
    lv_obj_t* kpiBlock = nullptr;
    lv_obj_t* kpiRow = nullptr;
    lv_obj_t* calibrationHeader = nullptr;
    lv_obj_t* chartsColumn = nullptr;
    lv_obj_t* vBlock = nullptr;
    lv_obj_t* iBlock = nullptr;
    lv_obj_t* vEditIcon = nullptr;
    lv_obj_t* iEditIcon = nullptr;
    lv_obj_t* oldLegend = nullptr;
    lv_obj_t* oldValueLabel = nullptr;
    lv_obj_t* newLegend = nullptr;
    lv_obj_t* newValueLabel = nullptr;
    lv_obj_t* newValueInput = nullptr;
    lv_obj_t* oldUnitLabel = nullptr;
    lv_obj_t* newUnitLabel = nullptr;
    lv_obj_t* vChart = nullptr;
    lv_obj_t* iChart = nullptr;
    lv_chart_series_t* vSeries = nullptr;
    lv_chart_series_t* iSeries = nullptr;
    lv_chart_series_t* vPreviewSeries = nullptr;
    lv_chart_series_t* iPreviewSeries = nullptr;
    SensorChartFrame vFrame;
    SensorChartFrame iFrame;

    lv_obj_t* vValueLabel = nullptr;
    lv_obj_t* iValueLabel = nullptr;
    lv_obj_t* pValueLabel = nullptr;
    lv_obj_t* statusLabel = nullptr;
    lv_obj_t* captureButton = nullptr;

    lv_obj_t* dutyRow = nullptr;    // hidden until duty is observed < threshold
    lv_obj_t* dutyValueLabel = nullptr;
    bool dutyEverLow = false;

    uint32_t lastTimestamp = 0;
    bool primed = false;

    uint32_t lastKpiTimestamp = 0;
    bool kpiPrimed = false;

    AxisRangeState vRange;
    AxisRangeState iRange;
    lv_obj_t* calibrationParent = nullptr;

    struct RawCaptureView {
        lv_obj_t* root = nullptr;
        lv_obj_t* statusLabel = nullptr;
        lv_obj_t* voltageTitle = nullptr;
        lv_obj_t* currentTitle = nullptr;
        lv_obj_t* summaryLabel = nullptr;
        lv_obj_t* windowStateLabel = nullptr;
        lv_obj_t* voltageChart = nullptr;
        lv_obj_t* currentChart = nullptr;
        lv_chart_series_t* voltageSeries = nullptr;
        lv_chart_series_t* currentSeries = nullptr;
        RawCaptureChartFrame voltageFrame;
        RawCaptureChartFrame currentFrame;
        uint32_t requestId = 0;
        bool visible = false;
        bool ownsRequest = false;
    } capture;

    struct CalibrationEditor {
        lv_obj_t* root = nullptr;
        lv_obj_t* offsetInput = nullptr;
        lv_obj_t* gainInput = nullptr;
        lv_obj_t* keyboard = nullptr;
        lv_obj_t* offsetRow = nullptr;
        lv_obj_t* actionRow = nullptr;
        lv_obj_t* calculationActionRow = nullptr;
        lv_obj_t* activeBlock = nullptr;
        lv_obj_t* activeTitleRow = nullptr;
        lv_obj_t* activeChart = nullptr;
        lv_chart_series_t* preview = nullptr;
        sensors::calibration::Measurement measurement = sensors::calibration::Measurement::Voltage;
        sensors::calibration::Value saved{};
        sensors::calibration::Value staged{};
        uint8_t sensor = 0;
        bool visible = false;
        bool refreshingInputs = false;
        bool calculatingGain = false;
        uint32_t lastPreviewTimestamp = 0;
    } calibration;
};

SensorTab sensorTabs[sensors::SENSOR_COUNT];

lv_timer_t* updateTimer = nullptr;
lv_timer_t* captureUpdateTimer = nullptr;

enum class CalibrationAction : uint8_t {
    Back, OffsetDown, OffsetUp, GainDown, GainUp, AutoZero, CalculateGain,
    Cancel, Save, Reset, ConfirmGain, CancelGain
};
struct CalibrationControl { SensorTab* tab; uint8_t sensor; CalibrationAction action; };
CalibrationControl calibrationControls[sensors::SENSOR_COUNT][12];

const char* measurementUnit(sensors::calibration::Measurement measurement) {
    return measurement == sensors::calibration::Measurement::Voltage ? "V" : "A";
}

sensors::calibration::Source activeCalibrationSource() {
    return sensor_mode::get() == sensor_mode::Mode::Ads1115
        ? sensors::calibration::Source::Ads1115
        : sensors::calibration::Source::Esp32Adc;
}

bool calibrationPhysicalSensor(uint8_t logicalSensor, uint8_t& physicalSensor) {
    sensors::mapping::PhysicalSensorId physical;
    if (!sensors::mapping::physicalForLogical(
            sensor_mode::get(), static_cast<sensors::SensorId>(logicalSensor),
            physical)) {
        return false;
    }
    physicalSensor = static_cast<uint8_t>(physical);
    return true;
}

float applyLogicalCurrentDirection(uint8_t logicalSensor, float current) {
    return current * static_cast<float>(
        sensors::mapping::currentMultiplierForLogical(
            sensor_mode::get(),
            static_cast<sensors::SensorId>(logicalSensor)));
}

float calibratedPreview(uint8_t sensor,
                        sensors::calibration::Measurement measurement,
                        float input,
                        sensors::calibration::Value calibration) {
    const float value = sensors::calibration::apply(input, calibration);
    return measurement == sensors::calibration::Measurement::Current
        ? applyLogicalCurrentDirection(sensor, value)
        : value;
}

float calibrationInputFromDisplayed(uint8_t sensor,
                                    sensors::calibration::Measurement measurement,
                                    float displayed,
                                    sensors::calibration::Value calibration) {
    const float sourceValue =
        measurement == sensors::calibration::Measurement::Current
            ? applyLogicalCurrentDirection(sensor, displayed)
            : displayed;
    return sourceValue / calibration.gain + calibration.offsetInputV;
}

bool rawCaptureAvailable() {
    const sensor_mode::Mode mode = sensor_mode::get();
    return mode == sensor_mode::Mode::Adc || mode == sensor_mode::Mode::Ads1115;
}

void updateCalibrationEditor(SensorTab& tab, uint8_t sensor,
                             const sensors::Reading* readings, size_t n,
                             bool appendPoint);
void refreshCalibrationEditor(SensorTab& tab, uint8_t sensor);
void refreshCalibrationInputs(SensorTab::CalibrationEditor& editor);
void openVoltageCalibrationCb(lv_event_t* event);
void openCurrentCalibrationCb(lv_event_t* event);
void closeCalibration(SensorTab& tab);
void openRawCaptureCb(lv_event_t* event);
void closeRawCapture(SensorTab& tab);
void createRawCaptureView(lv_obj_t* parent, SensorTab& tab, uint8_t sensor);

bool latestCalibrationInput(const SensorTab::CalibrationEditor& editor, float& input) {
    sensors::Reading latest{};
    if (!sensors::getLatest(static_cast<sensors::SensorId>(editor.sensor), latest)) return false;
    if (sensor_mode::usesCalibration(sensor_mode::get())) {
        input = editor.measurement == sensors::calibration::Measurement::Voltage
            ? latest.voltageInputV : latest.currentInputV;
    } else {
        const float displayed = editor.measurement == sensors::calibration::Measurement::Voltage
            ? latest.voltage : latest.current;
        input = calibrationInputFromDisplayed(
            editor.sensor, editor.measurement, displayed, editor.saved);
    }
    return std::isfinite(input);
}

void showGainCalculationInput(SensorTab& tab, bool show) {
    auto& editor = tab.calibration;
    if (show) {
        lv_obj_add_flag(tab.newValueLabel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(tab.newValueInput, LV_OBJ_FLAG_HIDDEN);
        if (editor.offsetRow) lv_obj_add_flag(editor.offsetRow, LV_OBJ_FLAG_HIDDEN);
        if (editor.calculationActionRow) lv_obj_clear_flag(editor.calculationActionRow, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(tab.newValueLabel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(tab.newValueInput, LV_OBJ_FLAG_HIDDEN);
        if (editor.offsetRow) lv_obj_clear_flag(editor.offsetRow, LV_OBJ_FLAG_HIDDEN);
        if (editor.calculationActionRow) lv_obj_add_flag(editor.calculationActionRow, LV_OBJ_FLAG_HIDDEN);
    }
}

bool calculateCalibrationGain(SensorTab::CalibrationEditor& editor) {
    char* end = nullptr;
    const char* text = lv_textarea_get_text(sensorTabs[editor.sensor].newValueInput);
    const float reference = strtof(text, &end);
    const float max = editor.measurement == sensors::calibration::Measurement::Voltage
        ? sensors::calibration::kVoltageMaxV : sensors::calibration::kCurrentMaxA;
    float input = NAN;
    if (!end || end == text || *end != '\0' || !std::isfinite(reference) || reference <= 0.0f ||
        reference > max || !latestCalibrationInput(editor, input)) return false;
    const float sourceDenominator = input - editor.staged.offsetInputV;
    const float denominator =
        editor.measurement == sensors::calibration::Measurement::Current
            ? applyLogicalCurrentDirection(editor.sensor, sourceDenominator)
            : sourceDenominator;
    if (fabsf(denominator) < 0.005f) return false;
    sensors::calibration::Value candidate = editor.staged;
    candidate.gain = reference / denominator;
    if (!sensors::calibration::isValid(editor.measurement, candidate)) return false;
    editor.staged = candidate;
    return true;
}

void finishCalibrationKeyboard(SensorTab::CalibrationEditor& editor, bool confirm) {
    SensorTab& tab = sensorTabs[editor.sensor];
    if (editor.calculatingGain) {
        if (confirm) calculateCalibrationGain(editor);
        editor.calculatingGain = false;
        showGainCalculationInput(tab, false);
    }
    if (editor.keyboard) {
        lv_obj_del(editor.keyboard);
        editor.keyboard = nullptr;
    }
    if (editor.activeBlock) lv_obj_clear_flag(editor.activeBlock, LV_OBJ_FLAG_HIDDEN);
    if (editor.actionRow) lv_obj_clear_flag(editor.actionRow, LV_OBJ_FLAG_HIDDEN);
    refreshCalibrationInputs(editor);
    refreshCalibrationEditor(tab, editor.sensor);
}

void hideCalibrationKeyboard(lv_event_t* event) {
    auto* editor = static_cast<SensorTab::CalibrationEditor*>(lv_event_get_user_data(event));
    if (!editor) return;
    finishCalibrationKeyboard(*editor, lv_event_get_code(event) == LV_EVENT_READY);
}

void showCalibrationKeyboard(SensorTab::CalibrationEditor& editor, lv_obj_t* input) {
    if (!editor.keyboard) {
        editor.keyboard = lv_keyboard_create(editor.root);
        lv_obj_set_size(editor.keyboard, lv_pct(100), 0);
        lv_obj_set_flex_grow(editor.keyboard, 1);
        lv_keyboard_set_mode(editor.keyboard, LV_KEYBOARD_MODE_NUMBER);
        lv_obj_add_event_cb(editor.keyboard, hideCalibrationKeyboard, LV_EVENT_READY, &editor);
        lv_obj_add_event_cb(editor.keyboard, hideCalibrationKeyboard, LV_EVENT_CANCEL, &editor);
    }
    // With a zero-height flex basis the keyboard consumes all remaining space,
    // keeping its bottom edge flush with the display instead of leaving a gap.
    if (editor.activeBlock) lv_obj_add_flag(editor.activeBlock, LV_OBJ_FLAG_HIDDEN);
    if (editor.actionRow) lv_obj_add_flag(editor.actionRow, LV_OBJ_FLAG_HIDDEN);
    lv_keyboard_set_textarea(editor.keyboard, input);
}

void calibrationInputFocusCb(lv_event_t* event) {
    auto* editor = static_cast<SensorTab::CalibrationEditor*>(lv_event_get_user_data(event));
    if (!editor) return;
    showCalibrationKeyboard(*editor, static_cast<lv_obj_t*>(lv_event_get_target(event)));
}

void refreshCalibrationInputs(SensorTab::CalibrationEditor& editor) {
    editor.refreshingInputs = true;
    char text[20];
    snprintf(text, sizeof(text), "%.3f", editor.staged.offsetInputV);
    lv_textarea_set_text(editor.offsetInput, text);
    // The UI uses the inverse sensitivity: ADC millivolts per displayed
    // volt/amp. It keeps values close to the divider/sensor data sheet.
    snprintf(text, sizeof(text), "%.3f", 1000.0f / editor.staged.gain);
    lv_textarea_set_text(editor.gainInput, text);
    editor.refreshingInputs = false;
}

void calibrationInputChangedCb(lv_event_t* event) {
    auto* editor = static_cast<SensorTab::CalibrationEditor*>(lv_event_get_user_data(event));
    if (!editor || editor->refreshingInputs) return;
    char* end = nullptr;
    const float value = strtof(lv_textarea_get_text(static_cast<lv_obj_t*>(lv_event_get_target(event))), &end);
    if (!end || end == lv_textarea_get_text(static_cast<lv_obj_t*>(lv_event_get_target(event))) || *end != '\0') return;
    if (lv_event_get_target(event) == editor->offsetInput) editor->staged.offsetInputV = value;
    else if (lv_event_get_target(event) == editor->gainInput && value > 0.0f) editor->staged.gain = 1000.0f / value;
    refreshCalibrationEditor(sensorTabs[editor->sensor], editor->sensor);
}

void calibrationControlCb(lv_event_t* event) {
    auto* control = static_cast<CalibrationControl*>(lv_event_get_user_data(event));
    if (!control) return;
    SensorTab& tab = *control->tab;
    auto& editor = tab.calibration;
    const uint8_t sensor = control->sensor;

    if (control->action == CalibrationAction::Back || control->action == CalibrationAction::Cancel) {
        closeCalibration(tab);
        return;
    }
    if (control->action == CalibrationAction::ConfirmGain) {
        finishCalibrationKeyboard(editor, true);
        return;
    }
    if (control->action == CalibrationAction::CancelGain) {
        finishCalibrationKeyboard(editor, false);
        return;
    }
    if (control->action == CalibrationAction::OffsetDown) editor.staged.offsetInputV -= 0.001f;
    if (control->action == CalibrationAction::OffsetUp) editor.staged.offsetInputV += 0.001f;
    // Match the Arduino UI's 0.001-per-press adjustment. Gain is presented
    // here as inverse sensitivity (mV per engineering unit), so adjust that
    // displayed value and convert it back to the stored multiplier.
    if (control->action == CalibrationAction::GainDown) {
        editor.staged.gain = 1000.0f / std::max(0.001f, 1000.0f / editor.staged.gain - 0.001f);
    }
    if (control->action == CalibrationAction::GainUp) {
        editor.staged.gain = 1000.0f / (1000.0f / editor.staged.gain + 0.001f);
    }
    if (control->action == CalibrationAction::AutoZero) {
        float input = NAN;
        if (latestCalibrationInput(editor, input)) {
            sensors::calibration::Value candidate = editor.staged;
            candidate.offsetInputV = input;
            if (sensors::calibration::isValid(editor.measurement, candidate)) editor.staged = candidate;
        }
    }
    if (control->action == CalibrationAction::CalculateGain) {
        lv_textarea_set_text(tab.newValueInput, lv_label_get_text(tab.newValueLabel));
        lv_textarea_set_cursor_pos(tab.newValueInput, LV_TEXTAREA_CURSOR_LAST);
        editor.calculatingGain = true;
        showGainCalculationInput(tab, true);
        showCalibrationKeyboard(editor, tab.newValueInput);
        return;
    }
    if (control->action == CalibrationAction::Reset) {
        uint8_t physicalSensor = 0;
        if (calibrationPhysicalSensor(sensor, physicalSensor)) {
            editor.staged = sensors::calibration::defaults(
                activeCalibrationSource(), physicalSensor, editor.measurement);
        }
    }
    if (control->action == CalibrationAction::Save) {
        uint8_t physicalSensor = 0;
        if (!sensor_mode::usesCalibration(sensor_mode::get()) ||
            (calibrationPhysicalSensor(sensor, physicalSensor) &&
             sensors::calibration::set(
                 activeCalibrationSource(), physicalSensor,
                 editor.measurement, editor.staged))) {
            editor.saved = editor.staged;
        }
    }
    refreshCalibrationInputs(editor);
    refreshCalibrationEditor(tab, sensor);
}

void closeCalibration(SensorTab& tab) {
    auto& editor = tab.calibration;
    if (!editor.visible) return;
    editor.visible = false;
    if (editor.keyboard) {
        lv_obj_del(editor.keyboard);
        editor.keyboard = nullptr;
    }
    if (editor.calculatingGain) {
        editor.calculatingGain = false;
        showGainCalculationInput(tab, false);
    }
    if (editor.preview && editor.activeChart) {
        lv_chart_set_all_value(editor.activeChart, editor.preview, LV_CHART_POINT_NONE);
        lv_chart_hide_series(editor.activeChart, editor.preview, true);
    }
    lv_chart_set_series_color(tab.vChart, tab.vSeries, lv_palette_main(LV_PALETTE_BLUE));
    lv_chart_set_series_color(tab.iChart, tab.iSeries, lv_palette_main(LV_PALETTE_ORANGE));
    editor.preview = nullptr;
    editor.activeChart = nullptr;
    if (editor.activeTitleRow) lv_obj_clear_flag(editor.activeTitleRow, LV_OBJ_FLAG_HIDDEN);
    editor.activeBlock = nullptr;
    editor.activeTitleRow = nullptr;
    lv_obj_clear_flag(tab.kpiBlock, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(tab.calibrationHeader, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(tab.vBlock, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(tab.iBlock, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(tab.vEditIcon, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(tab.iEditIcon, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_text_opa(tab.vEditIcon, LV_OPA_COVER, 0);
    lv_obj_set_style_text_opa(tab.iEditIcon, LV_OPA_COVER, 0);
    lv_obj_del(editor.root);
    editor.root = nullptr;
    editor.offsetRow = nullptr;
    editor.actionRow = nullptr;
    editor.calculationActionRow = nullptr;
}

void exitAllCalibrationCb(lv_event_t*) {
    for (SensorTab& tab : sensorTabs) {
        closeCalibration(tab);
        closeRawCapture(tab);
    }
}

// Given a batch of readings and where we left off last time, returns the
// index of the first reading that's actually new (or n if there's nothing
// new), and advances lastTimestamp/primed for next time.
size_t findStartAndAdvance(const sensors::Reading* readings, size_t n,
                            uint32_t& lastTimestamp, bool& primed) {
    if (n == 0) return 0;
    size_t start = 0;
    if (primed) {
        start = n; // assume nothing new unless we find it below
        for (size_t i = 0; i < n; i++) {
            if (readings[i].timestamp_ms > lastTimestamp) {
                start = i;
                break;
            }
        }
    } else {
        primed = true; // first successful update: draw the whole window we have
    }
    lastTimestamp = readings[n - 1].timestamp_ms;
    return start;
}

// ---------------------------------------------------------------------------
// Chart building helpers
// ---------------------------------------------------------------------------

// LVGL's chart tick drawing has varied slightly between 8.x point releases.
// Draw this small frame ourselves so every sensor chart has the same reliable
// labels and grid treatment as the Power screen.
void sensorChartGridDrawCb(lv_event_t* event) {
    if (lv_event_get_code(event) != LV_EVENT_DRAW_MAIN) return;
    auto* frame = static_cast<SensorChartFrame*>(lv_event_get_user_data(event));
    if (!frame || !frame->range || !frame->range->initialized) return;

    lv_draw_ctx_t* ctx = lv_event_get_draw_ctx(event);
    const lv_area_t& area = lv_event_get_target(event)->coords;
    const lv_coord_t left = area.x1 + 2;
    const lv_coord_t right = area.x2 - 3;
    const lv_coord_t top = area.y1 + 3;
    const lv_coord_t bottom = area.y2 - 3;
    const float span = frame->range->curMax - frame->range->curMin;
    if (span <= 0.0f || right <= left || bottom <= top) return;

    lv_draw_line_dsc_t grid;
    lv_draw_line_dsc_init(&grid);
    grid.color = ui_theme::border();
    grid.width = 1;
    grid.opa = LV_OPA_50;
    for (uint8_t i = 0; i <= 4; ++i) {
        const lv_coord_t y = top + (bottom - top) * i / 4;
        lv_point_t p1{left, y}, p2{right, y};
        lv_draw_line(ctx, &grid, &p1, &p2);
    }
    for (uint8_t i = 0; i <= kGridDivisions; ++i) {
        const lv_coord_t x = left + (right - left) * i / kGridDivisions;
        lv_point_t p1{x, top}, p2{x, bottom};
        lv_draw_line(ctx, &grid, &p1, &p2);
    }
}

void updateChartAxisLabels(SensorChartFrame& frame) {
    if (!frame.range || !frame.range->initialized) return;
    const float span = frame.range->curMax - frame.range->curMin;
    for (uint8_t i = 0; i <= 4; ++i) {
        if (!frame.axisLabels[i]) continue;
        char text[12];
        snprintf(text, sizeof(text), "%.1f", frame.range->curMax - span * i / 4.0f);
        lv_label_set_text(frame.axisLabels[i], text);
    }
}

void styleChartMinimal(lv_obj_t* chart) {
    lv_obj_set_style_border_width(chart, 0, 0);
    lv_obj_set_style_radius(chart, 0, 0);
    lv_obj_set_style_pad_all(chart, 2, 0);
    lv_obj_set_style_pad_column(chart, 0, 0);
    lv_obj_set_style_bg_opa(chart, LV_OPA_TRANSP, 0);

    // Thin, muted grid/div lines instead of the default heavy ones.
    lv_obj_set_style_line_width(chart, 1, LV_PART_MAIN);
    lv_obj_set_style_line_color(chart, ui_theme::border(), LV_PART_MAIN);
    lv_obj_set_style_line_opa(chart, LV_OPA_50, LV_PART_MAIN);

    // Trace: visible line, no per-point circles (cleaner at 500ms update rate).
    lv_obj_set_style_line_width(chart, 2, LV_PART_ITEMS);
    lv_obj_set_style_size(chart, 0, LV_PART_INDICATOR);

    lv_obj_set_style_text_font(chart, &lv_font_montserrat_14, LV_PART_TICKS);
    lv_obj_set_style_text_color(chart, ui_theme::mutedText(), LV_PART_TICKS);
}

static void styleFlatContainer(lv_obj_t *obj)
{
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_radius(obj, 0, 0);

    lv_obj_set_style_bg_color(obj, ui_theme::background(), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);

    lv_obj_set_style_shadow_width(obj, 0, 0);

    lv_obj_set_style_pad_all(obj, 0, 0);
}

// Adds a small "-30s / -20s / -10s / now" row under a chart so the person
// knows the time scale without needing live-updating axis labels - the
// window length is fixed, so static labels are accurate as long as the
// chart is actively scrolling.
void addTimeAxisLabels(lv_obj_t* parent) {
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    // Keep the endpoint labels inside the draw area; otherwise the final
    // "now" glyph can be clipped by the tab content boundary.
    lv_obj_set_style_pad_left(row, 2, 0);
    lv_obj_set_style_pad_right(row, 6, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    const uint32_t windowSec = kWindowMs / 1000;
    for (size_t i = 0; i <= kGridDivisions; i++) {
        char buf[8];
        uint32_t secAgo = windowSec - (i * (kGridIntervalMs / 1000));
        if (secAgo == 0) {
            snprintf(buf, sizeof(buf), "now");
        } else {
            snprintf(buf, sizeof(buf), "-%us", secAgo);
        }
        lv_obj_t* lbl = lv_label_create(row);
        lv_label_set_text(lbl, buf);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(lbl, ui_theme::mutedText(), 0);
    }
}

// Builds one minimally-styled chart block (title + chart + time axis) and
// wires up dynamic Y-axis ticks. Returns the chart object; writes the
// created series into *seriesOut.
lv_obj_t* createChartBlock(lv_obj_t* parent, const char* title, const char* unit,
                            lv_color_t color, lv_obj_t** blockOut,
                            lv_obj_t** valueLabelOut, lv_obj_t** editIconOut,
                            SensorChartFrame* frame, const float* axisScale,
                            lv_chart_series_t** seriesOut,
                            SensorTab* tab = nullptr, sensors::calibration::Measurement measurement = sensors::calibration::Measurement::Voltage) {
    lv_obj_t* block = lv_obj_create(parent);
    styleFlatContainer(block);
    // A zero base height lets the two blocks share all remaining tab height
    // rather than each claiming a full-height basis and leaving dead space.
    lv_obj_set_size(block, lv_pct(100), 0);
    lv_obj_set_style_border_width(block, 0, 0);
    lv_obj_set_style_pad_all(block, 0, 0);
    lv_obj_set_flex_flow(block, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_grow(block, 1);
    if (blockOut) *blockOut = block;

    if (title && title[0]) {
        lv_obj_t* titleRow = lv_obj_create(block);
        lv_obj_remove_style_all(titleRow);
        lv_obj_set_size(titleRow, lv_pct(100), 28);
        lv_obj_set_flex_flow(titleRow, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(titleRow, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_t* titleLabel = lv_label_create(titleRow);
        lv_label_set_text(titleLabel, title);
        lv_obj_set_width(titleLabel, 64);
        lv_obj_set_style_text_font(titleLabel, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(titleLabel, ui_theme::mutedText(), 0);

        lv_obj_t* valueLabel = lv_label_create(titleRow);
        lv_label_set_text(valueLabel, "--.-");
        lv_obj_set_width(valueLabel, 64);
        lv_label_set_long_mode(valueLabel, LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_align(valueLabel, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_style_text_font(valueLabel, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(valueLabel, ui_theme::text(), 0);
        if (valueLabelOut) *valueLabelOut = valueLabel;

        lv_obj_t* unitLabel = lv_label_create(titleRow);
        lv_label_set_text(unitLabel, unit);
        lv_obj_set_width(unitLabel, 14);
        lv_obj_set_style_text_font(unitLabel, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(unitLabel, ui_theme::mutedText(), 0);
        lv_obj_set_style_pad_top(unitLabel, 3, 0);

        if (tab) {
            // Let the calibration target consume the rest of the heading.
            // Its glyph remains pinned right while the live value sits beside
            // the chart title.
            lv_obj_t* editTarget = lv_obj_create(titleRow);
            lv_obj_remove_style_all(editTarget);
            lv_obj_set_size(editTarget, 0, lv_pct(100));
            lv_obj_set_flex_grow(editTarget, 1);
            lv_obj_add_flag(editTarget, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(editTarget,
                measurement == sensors::calibration::Measurement::Voltage ? openVoltageCalibrationCb : openCurrentCalibrationCb,
                LV_EVENT_CLICKED, tab);

            lv_obj_t* calibrate = lv_label_create(editTarget);
            lv_obj_set_size(calibrate, 28, 22);
            lv_obj_set_style_text_align(calibrate, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_style_text_color(calibrate, ui_theme::mutedText(), 0);
            lv_label_set_text(calibrate, LV_SYMBOL_EDIT);
            lv_obj_align(calibrate, LV_ALIGN_BOTTOM_RIGHT, 0, 2);
            if (editIconOut) *editIconOut = calibrate;
        }
    }

    lv_obj_t* chartRow = lv_obj_create(block);
    lv_obj_remove_style_all(chartRow);
    lv_obj_set_size(chartRow, lv_pct(100), 0);
    lv_obj_set_flex_grow(chartRow, 1);
    lv_obj_set_flex_flow(chartRow, LV_FLEX_FLOW_ROW);

    lv_obj_t* axis = lv_obj_create(chartRow);
    lv_obj_remove_style_all(axis);
    lv_obj_set_size(axis, 42, lv_pct(100));
    lv_obj_set_flex_flow(axis, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(axis, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    for (uint8_t i = 0; i <= 4; ++i) {
        frame->axisLabels[i] = lv_label_create(axis);
        lv_label_set_text(frame->axisLabels[i], "--");
        lv_obj_set_style_text_font(frame->axisLabels[i], &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(frame->axisLabels[i], ui_theme::mutedText(), 0);
    }

    lv_obj_t* chart = lv_chart_create(chartRow);
    lv_obj_set_size(chart, 0, lv_pct(100));
    lv_obj_set_flex_grow(chart, 1);
    lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
    lv_chart_set_update_mode(chart, LV_CHART_UPDATE_MODE_SHIFT);
    lv_chart_set_point_count(chart, kChartPoints);
    lv_chart_set_div_line_count(chart, 0, 0);
    lv_obj_set_style_clip_corner(chart, false, 0);

    styleChartMinimal(chart);
    frame->scale = *axisScale;
    lv_obj_add_event_cb(chart, sensorChartGridDrawCb, LV_EVENT_DRAW_MAIN, frame);

    *seriesOut = lv_chart_add_series(chart, color, LV_CHART_AXIS_PRIMARY_Y);

    addTimeAxisLabels(block);

    return chart;
}

// Small [value][unit] pair with a fixed-width value field so the unit label
// doesn't shift left/right as the number of digits changes.
lv_obj_t* createKpiItem(lv_obj_t* parent, const char* unit, lv_coord_t valueWidth, lv_obj_t** valueLabelOut) {
    lv_obj_t* item = lv_obj_create(parent);
    lv_obj_remove_style_all(item);
    lv_obj_set_size(item, LV_SIZE_CONTENT, 26);
    lv_obj_clear_flag(item, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(item, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_border_width(item, 0, 0);
    lv_obj_set_style_pad_all(item, 0, 0);
    lv_obj_set_style_pad_column(item, 3, 0);
    lv_obj_set_flex_flow(item, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(item, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);

    lv_obj_t* valueLabel = lv_label_create(item);
    lv_label_set_text(valueLabel, "--.-");
    lv_obj_set_style_text_font(valueLabel, &lv_font_montserrat_20, 0);
    lv_obj_set_width(valueLabel, valueWidth);
    lv_label_set_long_mode(valueLabel, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(valueLabel, LV_TEXT_ALIGN_RIGHT, 0);

    lv_obj_t* unitLabel = lv_label_create(item);
    lv_label_set_text(unitLabel, unit);
    lv_obj_set_style_text_font(unitLabel, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(unitLabel, ui_theme::mutedText(), 0);
    lv_obj_set_style_pad_bottom(unitLabel, 3, 0); // baseline-align with the larger value font

    *valueLabelOut = valueLabel;
    return item;
}

lv_obj_t* createCalibrationButton(lv_obj_t* parent, const char* text, CalibrationControl& control,
                                  SensorTab& tab, uint8_t sensor, CalibrationAction action, bool primary = false) {
    control = {&tab, sensor, action};
    lv_obj_t* button = lv_btn_create(parent);
    lv_obj_set_height(button, 32);
    lv_obj_set_flex_grow(button, 1);
    if (primary) ui_theme::stylePrimaryButton(button);
    else {
        lv_obj_set_style_bg_color(button, ui_theme::surfaceAlt(), 0);
        lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(button, ui_theme::accent(), 0);
        lv_obj_set_style_border_width(button, 1, 0);
        lv_obj_set_style_radius(button, 6, 0);
        lv_obj_set_style_text_color(button, ui_theme::accent(), 0);
        lv_obj_set_style_bg_color(button, ui_theme::accent(), LV_STATE_PRESSED);
        lv_obj_set_style_text_color(button, lv_color_white(), LV_STATE_PRESSED);
    }
    lv_obj_t* label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_16, 0);
    lv_obj_center(label);
    lv_obj_add_event_cb(button, calibrationControlCb, LV_EVENT_CLICKED, &control);
    return button;
}

void styleCalibrationRow(lv_obj_t* row) {
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), 36);
    lv_obj_set_style_pad_left(row, 4, 0);
    lv_obj_set_style_pad_right(row, 4, 0);
    lv_obj_set_style_pad_column(row, 4, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
}

lv_obj_t* createCalibrationRowLabel(lv_obj_t* row, const char* text) {
    lv_obj_t* label = lv_label_create(row);
    lv_label_set_text(label, text);
    lv_obj_set_width(label, 54);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_LEFT, 0);
    ui_theme::styleSectionLabel(label);
    return label;
}

void styleCalibrationInput(lv_obj_t* input) {
    lv_obj_set_size(input, 72, 30);
    lv_textarea_set_one_line(input, true);
    lv_obj_set_style_text_align(input, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_bg_color(input, ui_theme::surfaceAlt(), 0);
    lv_obj_set_style_bg_opa(input, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(input, ui_theme::border(), 0);
    lv_obj_set_style_border_width(input, 1, 0);
    lv_obj_set_style_border_color(input, ui_theme::accent(), LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(input, 2, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_width(input, 0, 0);
    lv_obj_set_style_radius(input, 5, 0);
    lv_obj_set_style_pad_top(input, 5, 0);
    lv_obj_set_style_pad_bottom(input, 4, 0);
    lv_obj_set_style_pad_left(input, 6, 0);
    lv_obj_set_style_pad_right(input, 6, 0);
}

void setCalibrationButtonWidth(lv_obj_t* button, lv_coord_t width) {
    lv_obj_set_flex_grow(button, 0);
    lv_obj_set_width(button, width);
}

lv_obj_t* createCalibrationUnit(lv_obj_t* row, const char* text) {
    lv_obj_t* unit = lv_label_create(row);
    lv_label_set_text(unit, text);
    lv_obj_set_width(unit, 42);
    lv_obj_set_style_text_align(unit, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_color(unit, ui_theme::mutedText(), 0);
    return unit;
}

void createCalibrationEditor(lv_obj_t* parent, SensorTab& tab, uint8_t sensor) {
    auto& editor = tab.calibration;
    editor.sensor = sensor;
    editor.keyboard = nullptr;
    editor.root = lv_obj_create(parent);
    styleFlatContainer(editor.root);
    lv_obj_set_size(editor.root, lv_pct(100), 0);
    lv_obj_set_flex_grow(editor.root, 1);
    lv_obj_set_flex_flow(editor.root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(editor.root, 4, 0);
    lv_obj_add_flag(editor.root, LV_OBJ_FLAG_HIDDEN);

    editor.offsetRow = lv_obj_create(editor.root);
    styleCalibrationRow(editor.offsetRow);
    createCalibrationRowLabel(editor.offsetRow, "Offset");
    setCalibrationButtonWidth(createCalibrationButton(editor.offsetRow, LV_SYMBOL_MINUS, calibrationControls[sensor][1], tab, sensor, CalibrationAction::OffsetDown), 34);
    editor.offsetInput = lv_textarea_create(editor.offsetRow);
    styleCalibrationInput(editor.offsetInput);
    lv_obj_add_event_cb(editor.offsetInput, calibrationInputFocusCb, LV_EVENT_FOCUSED, &editor);
    lv_obj_add_event_cb(editor.offsetInput, calibrationInputChangedCb, LV_EVENT_VALUE_CHANGED, &editor);
    createCalibrationUnit(editor.offsetRow, "V");
    setCalibrationButtonWidth(createCalibrationButton(editor.offsetRow, LV_SYMBOL_PLUS, calibrationControls[sensor][2], tab, sensor, CalibrationAction::OffsetUp), 34);
    setCalibrationButtonWidth(createCalibrationButton(editor.offsetRow, "Zero", calibrationControls[sensor][5], tab, sensor, CalibrationAction::AutoZero), 54);

    lv_obj_t* gainRow = lv_obj_create(editor.root);
    styleCalibrationRow(gainRow);
    createCalibrationRowLabel(gainRow, "Gain");
    setCalibrationButtonWidth(createCalibrationButton(gainRow, LV_SYMBOL_MINUS, calibrationControls[sensor][3], tab, sensor, CalibrationAction::GainDown), 34);
    editor.gainInput = lv_textarea_create(gainRow);
    styleCalibrationInput(editor.gainInput);
    lv_obj_add_event_cb(editor.gainInput, calibrationInputFocusCb, LV_EVENT_FOCUSED, &editor);
    lv_obj_add_event_cb(editor.gainInput, calibrationInputChangedCb, LV_EVENT_VALUE_CHANGED, &editor);
    createCalibrationUnit(gainRow, "mV/V");
    setCalibrationButtonWidth(createCalibrationButton(gainRow, LV_SYMBOL_PLUS, calibrationControls[sensor][4], tab, sensor, CalibrationAction::GainUp), 34);
    setCalibrationButtonWidth(createCalibrationButton(gainRow, "Calc", calibrationControls[sensor][6], tab, sensor, CalibrationAction::CalculateGain), 54);

    editor.calculationActionRow = lv_obj_create(editor.root);
    styleCalibrationRow(editor.calculationActionRow);
    createCalibrationButton(editor.calculationActionRow, "Enter", calibrationControls[sensor][10], tab, sensor, CalibrationAction::ConfirmGain, true);
    createCalibrationButton(editor.calculationActionRow, "Cancel", calibrationControls[sensor][11], tab, sensor, CalibrationAction::CancelGain);
    lv_obj_add_flag(editor.calculationActionRow, LV_OBJ_FLAG_HIDDEN);

    editor.actionRow = lv_obj_create(editor.root);
    styleCalibrationRow(editor.actionRow);
    createCalibrationButton(editor.actionRow, "Cancel", calibrationControls[sensor][7], tab, sensor, CalibrationAction::Cancel);
    createCalibrationButton(editor.actionRow, "Save", calibrationControls[sensor][8], tab, sensor, CalibrationAction::Save, true);
    createCalibrationButton(editor.actionRow, "Reset", calibrationControls[sensor][9], tab, sensor, CalibrationAction::Reset);

}

void openCalibration(SensorTab& tab, uint8_t sensor, sensors::calibration::Measurement measurement) {
    auto& editor = tab.calibration;
    uint8_t physicalSensor = 0;
    if (!calibrationPhysicalSensor(sensor, physicalSensor)) return;
    if (!editor.root) createCalibrationEditor(tab.chartsColumn, tab, sensor);
    editor.measurement = measurement;
    editor.saved = sensors::calibration::get(
        activeCalibrationSource(), physicalSensor, measurement);
    editor.staged = editor.saved;
    editor.visible = true;
    editor.calculatingGain = false;
    showGainCalculationInput(tab, false);
    const bool voltage = measurement == sensors::calibration::Measurement::Voltage;
    editor.activeBlock = voltage ? tab.vBlock : tab.iBlock;
    editor.activeChart = voltage ? tab.vChart : tab.iChart;
    lv_chart_set_series_color(editor.activeChart, voltage ? tab.vSeries : tab.iSeries,
                              lv_palette_main(LV_PALETTE_BLUE));
    editor.activeTitleRow = lv_obj_get_child(editor.activeBlock, 0);
    lv_obj_add_flag(tab.kpiBlock, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(tab.calibrationHeader, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(voltage ? tab.iBlock : tab.vBlock, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(editor.activeBlock, LV_OBJ_FLAG_HIDDEN);
    if (editor.activeTitleRow) lv_obj_add_flag(editor.activeTitleRow, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t* activeEditIcon = voltage ? tab.vEditIcon : tab.iEditIcon;
    lv_obj_add_flag(activeEditIcon, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_text_opa(activeEditIcon, LV_OPA_TRANSP, 0);
    refreshCalibrationInputs(editor);
    // Units are tied to the selected engineering measurement, while gain is
    // shown as ADC millivolts per engineering unit.
    lv_label_set_text(lv_obj_get_child(lv_obj_get_parent(editor.gainInput), 3),
                      measurement == sensors::calibration::Measurement::Voltage ? "mV/V" : "mV/A");
    lv_label_set_text(tab.oldUnitLabel, measurementUnit(measurement));
    lv_label_set_text(tab.newUnitLabel, measurementUnit(measurement));
    // New is intentionally append-only.  Factor changes affect readings that
    // arrive after the change; the blue live trace remains the stable history.
    editor.preview = voltage ? tab.vPreviewSeries : tab.iPreviewSeries;
    lv_chart_set_all_value(editor.activeChart, editor.preview, LV_CHART_POINT_NONE);
    lv_chart_hide_series(editor.activeChart, editor.preview, false);
    sensors::Reading latest{};
    editor.lastPreviewTimestamp = sensors::getLatest(static_cast<sensors::SensorId>(sensor), latest)
        ? latest.timestamp_ms : 0;
    if (editor.lastPreviewTimestamp) {
        const float oldValue = voltage ? latest.voltage : latest.current;
        const float input =
            sensor_mode::usesCalibration(sensor_mode::get())
                ? (voltage ? latest.voltageInputV : latest.currentInputV)
                : calibrationInputFromDisplayed(
                      sensor, measurement, oldValue, editor.saved);
        const float newValue =
            calibratedPreview(sensor, measurement, input, editor.staged);
        char text[12];
        snprintf(text, sizeof(text), "%.1f", oldValue); lv_label_set_text(tab.oldValueLabel, text);
        snprintf(text, sizeof(text), "%.1f", newValue); lv_label_set_text(tab.newValueLabel, text);
    }
    lv_obj_clear_flag(editor.root, LV_OBJ_FLAG_HIDDEN);
}

void openVoltageCalibrationCb(lv_event_t* event) {
    auto* tab = static_cast<SensorTab*>(lv_event_get_user_data(event));
    if (!tab) return;
    const auto logical = static_cast<sensors::SensorId>(tab - sensorTabs);
    sensors::mapping::PhysicalSensorId physical{};
    if (sensors::mapping::physicalForLogical(
            sensor_mode::get(), logical, physical)) {
        sensor_calibration_overlay::show(
            physical, sensors::calibration::Measurement::Voltage);
    }
}

void openCurrentCalibrationCb(lv_event_t* event) {
    auto* tab = static_cast<SensorTab*>(lv_event_get_user_data(event));
    if (!tab) return;
    const auto logical = static_cast<sensors::SensorId>(tab - sensorTabs);
    sensors::mapping::PhysicalSensorId physical{};
    if (sensors::mapping::physicalForLogical(
            sensor_mode::get(), logical, physical)) {
        sensor_calibration_overlay::show(
            physical, sensors::calibration::Measurement::Current);
    }
}

const char* sensorName(uint8_t sensor) {
    static constexpr const char* names[sensors::SENSOR_COUNT] = {"Solar", "Load", "Battery"};
    return sensor < sensors::SENSOR_COUNT ? names[sensor] : "Sensor";
}

const char* captureReadingStateLabel(sensors::ReadingState state) {
    switch (state) {
        case sensors::ReadingState::NotConfigured: return "Not configured";
        case sensors::ReadingState::Waiting: return "Waiting";
        case sensors::ReadingState::Valid: return "Good";
        case sensors::ReadingState::OutOfRange: return "Out of range";
        case sensors::ReadingState::Invalid: return "Invalid";
        case sensors::ReadingState::Stale: return "Stale";
    }
    return "Invalid";
}

void rawCaptureGridDrawCb(lv_event_t* event) {
    if (lv_event_get_code(event) != LV_EVENT_DRAW_MAIN) return;
    auto* frame = static_cast<RawCaptureChartFrame*>(lv_event_get_user_data(event));
    if (!frame) return;

    lv_draw_ctx_t* ctx = lv_event_get_draw_ctx(event);
    const lv_area_t& area = lv_event_get_target(event)->coords;
    const lv_coord_t left = area.x1 + 2;
    const lv_coord_t right = area.x2 - 3;
    const lv_coord_t top = area.y1 + 3;
    const lv_coord_t bottom = area.y2 - 3;
    if (right <= left || bottom <= top) return;

    lv_draw_line_dsc_t grid;
    lv_draw_line_dsc_init(&grid);
    grid.color = ui_theme::border();
    grid.width = 1;
    grid.opa = LV_OPA_50;
    for (uint8_t i = 0; i < 4; ++i) {
        const lv_coord_t y = top + (bottom - top) * i / 3;
        lv_point_t p1{left, y}, p2{right, y};
        lv_draw_line(ctx, &grid, &p1, &p2);
    }

    const uint16_t denominator = frame->pointCount > 1
        ? frame->pointCount - 1 : 1;
    for (uint8_t i = 0; i < frame->boundaryCount; ++i) {
        const uint16_t point = std::min<uint16_t>(
            frame->boundaries[i], denominator);
        const lv_coord_t x = left +
            static_cast<lv_coord_t>((right - left) * point / denominator);
        lv_point_t p1{x, top}, p2{x, bottom};
        lv_draw_line(ctx, &grid, &p1, &p2);
    }
}

lv_obj_t* createRawCaptureChart(lv_obj_t* parent, lv_color_t color,
                                const char* unit, RawCaptureChartFrame& frame,
                                lv_obj_t** titleOut, lv_chart_series_t** seriesOut) {
    lv_obj_t* block = lv_obj_create(parent);
    styleFlatContainer(block);
    lv_obj_set_size(block, lv_pct(100), 0);
    lv_obj_set_flex_grow(block, 1);
    lv_obj_set_flex_flow(block, LV_FLEX_FLOW_COLUMN);

    lv_obj_t* title = lv_label_create(block);
    lv_obj_set_width(title, lv_pct(100));
    lv_label_set_long_mode(title, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(title, ui_theme::mutedText(), 0);
    lv_label_set_text(title, "Waiting for capture");
    *titleOut = title;

    lv_obj_t* chartRow = lv_obj_create(block);
    lv_obj_remove_style_all(chartRow);
    lv_obj_set_size(chartRow, lv_pct(100), 0);
    lv_obj_set_flex_grow(chartRow, 1);
    lv_obj_set_flex_flow(chartRow, LV_FLEX_FLOW_ROW);

    frame.unit = unit;
    lv_obj_t* axis = lv_obj_create(chartRow);
    lv_obj_remove_style_all(axis);
    lv_obj_set_size(axis, 50, lv_pct(100));
    lv_obj_set_flex_flow(axis, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(axis, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    for (uint8_t i = 0; i < 4; ++i) {
        frame.axisLabels[i] = lv_label_create(axis);
        char label[12];
        snprintf(label, sizeof(label), "-- %s", unit);
        lv_label_set_text(frame.axisLabels[i], label);
        lv_obj_set_style_text_font(frame.axisLabels[i], &lv_font_montserrat_10, 0);
        lv_obj_set_style_text_color(frame.axisLabels[i], ui_theme::mutedText(), 0);
    }

    lv_obj_t* chart = lv_chart_create(chartRow);
    lv_obj_set_size(chart, 0, lv_pct(100));
    lv_obj_set_flex_grow(chart, 1);
    lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
    lv_chart_set_update_mode(chart, LV_CHART_UPDATE_MODE_SHIFT);
    lv_chart_set_point_count(chart, 2);
    lv_chart_set_div_line_count(chart, 0, 0);
    lv_obj_set_style_clip_corner(chart, false, 0);
    styleChartMinimal(chart);
    lv_obj_add_event_cb(chart, rawCaptureGridDrawCb, LV_EVENT_DRAW_MAIN, &frame);
    *seriesOut = lv_chart_add_series(chart, color, LV_CHART_AXIS_PRIMARY_Y);
    return chart;
}

void formatCaptureValue(char* out, size_t size, float value, uint8_t decimals) {
    if (!std::isfinite(value)) {
        snprintf(out, size, "--");
        return;
    }
    snprintf(out, size, decimals ? "%.1f" : "%.0f", value);
}

void setRawCaptureTitle(lv_obj_t* label, const char* name, const char* unit,
                        const sensors::AdcCaptureResult& result, bool voltage) {
    char values[sensors::kAdcCaptureWindowCount][12];
    for (uint8_t i = 0; i < sensors::kAdcCaptureWindowCount; ++i) {
        const float value = i < result.windowCount
            ? (voltage ? result.windows[i].reading.voltage
                       : result.windows[i].reading.current)
            : NAN;
        formatCaptureValue(values[i], sizeof(values[i]), value, 1);
    }
    char text[96];
    snprintf(text, sizeof(text), "%s 500ms: %s / %s / %s %s",
             name, values[0], values[1], values[2], unit);
    lv_label_set_text(label, text);
}

void populateRawCaptureChart(lv_obj_t* chart, lv_chart_series_t* series,
                             RawCaptureChartFrame& frame,
                             const sensors::AdcCaptureResult& result, bool voltage) {
    const uint16_t points = std::max<uint16_t>(2, result.pointCount);
    lv_chart_set_point_count(chart, points);
    lv_chart_set_all_value(chart, series, LV_CHART_POINT_NONE);

    float minimum = INFINITY;
    float maximum = -INFINITY;
    for (uint16_t i = 0; i < result.pointCount; ++i) {
        const float value = voltage ? result.points[i].voltage : result.points[i].current;
        if (std::isfinite(value)) {
            minimum = std::min(minimum, value);
            maximum = std::max(maximum, value);
        }
        lv_chart_set_next_value(chart, series,
            chartCoordinate(value, voltage ? kVoltageAxisScale : kCurrentAxisScale));
    }
    for (uint16_t i = result.pointCount; i < points; ++i) {
        lv_chart_set_next_value(chart, series, LV_CHART_POINT_NONE);
    }

    if (!std::isfinite(minimum) || !std::isfinite(maximum)) {
        minimum = 0.0f;
        maximum = 1.0f;
    }
    const float minimumSpan = voltage ? 0.5f : 0.25f;
    if (maximum - minimum < minimumSpan) {
        const float middle = (maximum + minimum) * 0.5f;
        minimum = middle - minimumSpan * 0.5f;
        maximum = middle + minimumSpan * 0.5f;
    }
    const float step = niceStep((maximum - minimum) / 3.0f);
    minimum = floorf(minimum / step) * step;
    maximum = ceilf(maximum / step) * step;
    const float scale = voltage ? kVoltageAxisScale : kCurrentAxisScale;
    lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y,
                       axisCoordinate(minimum, scale), axisCoordinate(maximum, scale));
    frame.minimum = minimum;
    frame.maximum = maximum;
    frame.scale = scale;
    frame.pointCount = result.pointCount;
    frame.boundaryCount = std::min<uint8_t>(
        result.windowCount + 1, sensors::kAdcCaptureWindowCount + 1);
    for (uint8_t i = 0; i < result.windowCount; ++i) {
        frame.boundaries[i] = result.windows[i].firstPoint;
    }
    if (frame.boundaryCount) {
        frame.boundaries[frame.boundaryCount - 1] =
            result.pointCount > 0 ? result.pointCount - 1 : 0;
    }
    const float span = maximum - minimum;
    for (uint8_t i = 0; i < 4; ++i) {
        char label[16];
        snprintf(label, sizeof(label), "%.1f %s",
                 maximum - span * i / 3.0f, frame.unit);
        lv_label_set_text(frame.axisLabels[i], label);
    }
    lv_chart_refresh(chart);
}

void resetRawCaptureFrame(RawCaptureChartFrame& frame) {
    frame.pointCount = 0;
    frame.boundaryCount = 0;
    for (lv_obj_t* label : frame.axisLabels) {
        if (!label) continue;
        char text[12];
        snprintf(text, sizeof(text), "-- %s", frame.unit);
        lv_label_set_text(label, text);
    }
}

void showRawCaptureResult(SensorTab& tab, const sensors::AdcCaptureResult& result) {
    auto& capture = tab.capture;
    capture.ownsRequest = false;
    capture.requestId = 0;
    const float frequency = result.measuredIntervalUs
        ? 1000000.0f / static_cast<float>(result.measuredIntervalUs) : NAN;
    char status[64];
    if (std::isfinite(frequency)) {
        snprintf(status, sizeof(status), "%u samples  %.1f Hz  %u dropped",
                 result.pointCount, frequency, result.droppedPoints);
    } else {
        snprintf(status, sizeof(status), "%u samples  rate unavailable",
                 result.pointCount);
    }
    lv_label_set_text(capture.statusLabel, status);

    setRawCaptureTitle(capture.voltageTitle, "Voltage", "V", result, true);
    setRawCaptureTitle(capture.currentTitle, "Current", "A", result, false);
    populateRawCaptureChart(capture.voltageChart, capture.voltageSeries,
                            capture.voltageFrame, result, true);
    populateRawCaptureChart(capture.currentChart, capture.currentSeries,
                            capture.currentFrame, result, false);

    char windowStates[112];
    snprintf(windowStates, sizeof(windowStates), "Windows: %s / %s / %s",
             result.windowCount > 0
                 ? captureReadingStateLabel(result.windows[0].reading.state) : "--",
             result.windowCount > 1
                 ? captureReadingStateLabel(result.windows[1].reading.state) : "--",
             result.windowCount > 2
                 ? captureReadingStateLabel(result.windows[2].reading.state) : "--");
    lv_label_set_text(capture.windowStateLabel, windowStates);

    char power[sensors::kAdcCaptureWindowCount][12];
    char duty[sensors::kAdcCaptureWindowCount][12];
    for (uint8_t i = 0; i < sensors::kAdcCaptureWindowCount; ++i) {
        const bool present = i < result.windowCount;
        formatCaptureValue(power[i], sizeof(power[i]),
                           present ? result.windows[i].reading.power : NAN, 0);
        const sensors::Reading& reading = result.windows[i].reading;
        formatCaptureValue(duty[i], sizeof(duty[i]),
                           present && reading.dutyState == sensors::DutyState::Valid
                               ? reading.dutyCycle * 100.0f : NAN, 0);
    }
    char summary[112];
    if (hardware_profile::kControllerIsPwm) {
        snprintf(summary, sizeof(summary), "Power %s / %s / %s W   Duty %s / %s / %s %%",
                 power[0], power[1], power[2], duty[0], duty[1], duty[2]);
    } else {
        snprintf(summary, sizeof(summary), "Power %s / %s / %s W",
                 power[0], power[1], power[2]);
    }
    lv_label_set_text(capture.summaryLabel, summary);
}

void beginRawCapture(SensorTab& tab) {
    const uint8_t sensor = static_cast<uint8_t>(&tab - sensorTabs);
    if (!tab.capture.root) createRawCaptureView(tab.contentRoot, tab, sensor);
    auto& capture = tab.capture;
    if (capture.ownsRequest) sensors::cancelAdcCapture(capture.requestId);
    capture.visible = true;
    capture.ownsRequest = false;
    capture.requestId = 0;
    lv_obj_add_flag(tab.kpiBlock, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(tab.chartsColumn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(capture.root, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(capture.statusLabel, "Arming capture...");
    lv_label_set_text(capture.voltageTitle, "Voltage");
    lv_label_set_text(capture.currentTitle, "Current");
    lv_label_set_text(capture.summaryLabel, "");
    lv_label_set_text(capture.windowStateLabel, "");
    lv_chart_set_all_value(capture.voltageChart, capture.voltageSeries, LV_CHART_POINT_NONE);
    lv_chart_set_all_value(capture.currentChart, capture.currentSeries, LV_CHART_POINT_NONE);
    resetRawCaptureFrame(capture.voltageFrame);
    resetRawCaptureFrame(capture.currentFrame);
    lv_chart_refresh(capture.voltageChart);
    lv_chart_refresh(capture.currentChart);

    if (!rawCaptureAvailable()) {
        lv_label_set_text(capture.statusLabel, "Raw capture unavailable for this source");
        return;
    }
    sensors::Reading latest{};
    if (sensors::getLatest(static_cast<sensors::SensorId>(sensor), latest) &&
        !sensors::isConfigured(latest)) {
        lv_label_set_text(capture.statusLabel, "This sensor is not configured");
        return;
    }
    capture.ownsRequest = sensors::requestAdcCapture(
        static_cast<sensors::SensorId>(sensor), capture.requestId);
    if (!capture.ownsRequest) {
        lv_label_set_text(capture.statusLabel, "Another capture is already active");
        return;
    }
    lv_label_set_text(capture.statusLabel, "Waiting for the next 500 ms window...");
}

void openRawCaptureCb(lv_event_t* event) {
    auto* tab = static_cast<SensorTab*>(lv_event_get_user_data(event));
    if (tab) beginRawCapture(*tab);
}

void closeRawCaptureCb(lv_event_t* event) {
    auto* tab = static_cast<SensorTab*>(lv_event_get_user_data(event));
    if (tab) closeRawCapture(*tab);
}

void closeRawCapture(SensorTab& tab) {
    auto& capture = tab.capture;
    if (!capture.visible) return;
    if (capture.ownsRequest) sensors::cancelAdcCapture(capture.requestId);
    capture.ownsRequest = false;
    capture.requestId = 0;
    capture.visible = false;
    lv_chart_set_point_count(capture.voltageChart, 2);
    lv_chart_set_point_count(capture.currentChart, 2);
    lv_chart_set_all_value(capture.voltageChart, capture.voltageSeries, LV_CHART_POINT_NONE);
    lv_chart_set_all_value(capture.currentChart, capture.currentSeries, LV_CHART_POINT_NONE);
    resetRawCaptureFrame(capture.voltageFrame);
    resetRawCaptureFrame(capture.currentFrame);
    lv_obj_add_flag(capture.root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(tab.kpiBlock, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(tab.chartsColumn, LV_OBJ_FLAG_HIDDEN);
}

void createRawCaptureView(lv_obj_t* parent, SensorTab& tab, uint8_t sensor) {
    auto& capture = tab.capture;
    capture.root = lv_obj_create(parent);
    styleFlatContainer(capture.root);
    lv_obj_set_size(capture.root, lv_pct(100), 0);
    lv_obj_set_flex_grow(capture.root, 1);
    lv_obj_set_flex_flow(capture.root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(capture.root, 3, 0);

    lv_obj_t* header = lv_obj_create(capture.root);
    styleFlatContainer(header);
    lv_obj_set_size(header, lv_pct(100), 30);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* back = lv_label_create(header);
    lv_label_set_text(back, LV_SYMBOL_LEFT);
    lv_obj_set_size(back, 36, 30);
    lv_obj_set_style_pad_top(back, 5, 0);
    lv_obj_set_style_text_align(back, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(back, &lv_font_montserrat_20, 0);
    lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(back, closeRawCaptureCb, LV_EVENT_CLICKED, &tab);

    lv_obj_t* title = lv_label_create(header);
    char titleText[24];
    snprintf(titleText, sizeof(titleText), "%s Raw", sensorName(sensor));
    lv_label_set_text(title, titleText);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);

    lv_obj_t* refresh = lv_label_create(header);
    lv_label_set_text(refresh, LV_SYMBOL_REFRESH);
    lv_obj_set_size(refresh, 36, 30);
    lv_obj_set_style_pad_top(refresh, 5, 0);
    lv_obj_set_style_text_align(refresh, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(refresh, &lv_font_montserrat_20, 0);
    lv_obj_add_flag(refresh, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(refresh, openRawCaptureCb, LV_EVENT_CLICKED, &tab);

    capture.statusLabel = lv_label_create(capture.root);
    lv_obj_set_width(capture.statusLabel, lv_pct(100));
    lv_obj_set_style_text_align(capture.statusLabel, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(capture.statusLabel, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(capture.statusLabel, ui_theme::mutedText(), 0);

    capture.windowStateLabel = lv_label_create(capture.root);
    lv_obj_set_width(capture.windowStateLabel, lv_pct(100));
    lv_label_set_long_mode(capture.windowStateLabel, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(capture.windowStateLabel, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(capture.windowStateLabel, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(capture.windowStateLabel, ui_theme::mutedText(), 0);

    capture.voltageChart = createRawCaptureChart(
        capture.root, lv_palette_main(LV_PALETTE_BLUE), "V", capture.voltageFrame,
        &capture.voltageTitle, &capture.voltageSeries);
    capture.currentChart = createRawCaptureChart(
        capture.root, lv_palette_main(LV_PALETTE_ORANGE), "A", capture.currentFrame,
        &capture.currentTitle, &capture.currentSeries);

    capture.summaryLabel = lv_label_create(capture.root);
    lv_obj_set_width(capture.summaryLabel, lv_pct(100));
    lv_label_set_long_mode(capture.summaryLabel, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(capture.summaryLabel, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(capture.summaryLabel, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(capture.summaryLabel, ui_theme::mutedText(), 0);
    lv_obj_add_flag(capture.root, LV_OBJ_FLAG_HIDDEN);
}

lv_obj_t* createSensorTab(lv_obj_t* tabParent, uint8_t sensorIndex) {
    lv_obj_set_style_bg_color(tabParent, ui_theme::background(), 0);
    lv_obj_set_style_bg_opa(tabParent, LV_OPA_COVER, 0);

    lv_obj_set_style_border_width(tabParent, 0, 0);
    lv_obj_set_style_radius(tabParent, 0, 0);
    lv_obj_set_style_shadow_width(tabParent, 0, 0);
    lv_obj_set_style_pad_all(tabParent, 0, 0);

    lv_obj_t* tab = lv_obj_create(tabParent);
    styleFlatContainer(tab);
    lv_obj_set_size(tab, lv_pct(100), lv_pct(100));
    lv_obj_set_style_pad_all(tab, 2, 0);
    lv_obj_set_style_pad_row(tab, 4, 0);
    lv_obj_set_style_border_width(tab, 0, 0);
    lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);

    SensorTab& t = sensorTabs[sensorIndex];
    t.calibrationParent = tabParent;
    t.contentRoot = tab;

    // --- KPI row -----------------------------------------------------------
    // Voltage and current live in their chart headings. Keep this strip for
    // power, conditional duty, a flexible warning, and the capture action.
    t.kpiBlock = lv_obj_create(tab);
    styleFlatContainer(t.kpiBlock);
    lv_obj_set_size(t.kpiBlock, lv_pct(100), 30);
    lv_obj_clear_flag(t.kpiBlock, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(t.kpiBlock, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(t.kpiBlock, LV_FLEX_FLOW_COLUMN);

    t.kpiRow = lv_obj_create(t.kpiBlock);
    styleFlatContainer(t.kpiRow);
    lv_obj_set_size(t.kpiRow, lv_pct(100), 30);
    lv_obj_clear_flag(t.kpiRow, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(t.kpiRow, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_border_width(t.kpiRow, 0, 0);
    lv_obj_set_style_pad_all(t.kpiRow, 0, 0);
    lv_obj_set_style_pad_column(t.kpiRow, 4, 0);
    lv_obj_set_flex_flow(t.kpiRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(t.kpiRow, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    createKpiItem(t.kpiRow, "W", 64, &t.pValueLabel);
    lv_label_set_text(t.pValueLabel, "--");
    if (hardware_profile::kControllerIsPwm) {
        t.dutyRow = createKpiItem(t.kpiRow, "%", 48, &t.dutyValueLabel);
    }

    t.statusLabel = lv_label_create(t.kpiRow);
    lv_obj_set_width(t.statusLabel, 0);
    lv_obj_set_flex_grow(t.statusLabel, 1);
    lv_label_set_long_mode(t.statusLabel, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(t.statusLabel, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_font(t.statusLabel, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(
        t.statusLabel, lv_palette_main(LV_PALETTE_AMBER), 0);
    lv_label_set_text(t.statusLabel, LV_SYMBOL_WARNING " Waiting");
    t.captureButton = lv_btn_create(t.kpiRow);
    lv_obj_set_size(t.captureButton, 42, 24);
    lv_obj_set_style_pad_all(t.captureButton, 0, 0);
    lv_obj_set_style_radius(t.captureButton, 5, 0);
    lv_obj_set_style_shadow_width(t.captureButton, 0, 0);
    lv_obj_set_style_bg_color(t.captureButton, ui_theme::surfaceAlt(), 0);
    lv_obj_set_style_bg_opa(t.captureButton, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(t.captureButton, 1, 0);
    lv_obj_set_style_border_color(t.captureButton, ui_theme::accent(), 0);
    lv_obj_set_style_text_color(t.captureButton, ui_theme::accent(), 0);
    lv_obj_set_style_bg_color(
        t.captureButton, ui_theme::accent(), LV_STATE_PRESSED);
    lv_obj_set_style_text_color(
        t.captureButton, lv_color_white(), LV_STATE_PRESSED);
    lv_obj_add_event_cb(t.captureButton, openRawCaptureCb, LV_EVENT_CLICKED, &t);
    lv_obj_t* captureLabel = lv_label_create(t.captureButton);
    lv_label_set_text(captureLabel, "Raw");
    lv_obj_set_style_text_font(captureLabel, &lv_font_montserrat_12, 0);
    lv_obj_center(captureLabel);
    // Reveal only after the update loop has confirmed this channel is
    // configured for the active ADC source.
    lv_obj_add_flag(t.captureButton, LV_OBJ_FLAG_HIDDEN);
    if (t.dutyRow) {
        lv_obj_add_flag(t.dutyRow, LV_OBJ_FLAG_HIDDEN); // hidden until it's seen below threshold
    }

    // --- Voltage + current charts, stacked, sharing remaining space --------
    t.chartsColumn = lv_obj_create(tab);
    styleFlatContainer(t.chartsColumn);
    lv_obj_set_size(t.chartsColumn, lv_pct(100), 0);
    lv_obj_set_style_border_width(t.chartsColumn, 0, 0);
    lv_obj_set_style_pad_all(t.chartsColumn, 0, 0);
    lv_obj_set_style_pad_row(t.chartsColumn, 6, 0);
    lv_obj_set_flex_flow(t.chartsColumn, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_grow(t.chartsColumn, 1);

    t.vFrame.range = &t.vRange;
    t.iFrame.range = &t.iRange;
    t.vChart = createChartBlock(t.chartsColumn, "Voltage", "V",
                                 lv_palette_main(LV_PALETTE_BLUE), &t.vBlock,
                                 &t.vValueLabel, &t.vEditIcon, &t.vFrame,
                                 &kVoltageAxisScale, &t.vSeries, &t,
                                 sensors::calibration::Measurement::Voltage);
    t.iChart = createChartBlock(t.chartsColumn, "Current", "A",
                                 lv_palette_main(LV_PALETTE_ORANGE), &t.iBlock,
                                 &t.iValueLabel, &t.iEditIcon, &t.iFrame,
                                 &kCurrentAxisScale, &t.iSeries, &t,
                                 sensors::calibration::Measurement::Current);
    lv_chart_set_range(t.vChart, LV_CHART_AXIS_PRIMARY_Y,
                        (lv_coord_t)(kVoltageDefaultMin * kVoltageAxisScale),
                        (lv_coord_t)(kVoltageDefaultMax * kVoltageAxisScale));
    lv_chart_set_range(t.iChart, LV_CHART_AXIS_PRIMARY_Y,
                        (lv_coord_t)(kCurrentDefaultMin * kCurrentAxisScale),
                        (lv_coord_t)(kCurrentDefaultMax * kCurrentAxisScale));

    return tab;
}

void updateCalibrationEditor(SensorTab& tab, uint8_t sensor,
                             const sensors::Reading* readings, size_t n,
                             bool appendPoint) {
    auto& editor = tab.calibration;
    if (!editor.visible || !readings || n == 0) return;

    const bool voltage = editor.measurement == sensors::calibration::Measurement::Voltage;
    const auto inputFor = [&](const sensors::Reading& reading) {
        if (sensor_mode::usesCalibration(sensor_mode::get())) return voltage ? reading.voltageInputV : reading.currentInputV;
        // Simulation supplies engineering units, not ADC volts. Reconstruct a
        // compatible input so the preview graph remains meaningful for UI
        // walkthroughs without ever applying/saving demo calibration.
        const float value = voltage ? reading.voltage : reading.current;
        return calibrationInputFromDisplayed(
            sensor, editor.measurement, value, editor.saved);
    };
    const sensors::Reading& latest = readings[n - 1];
    const float oldValue = voltage ? latest.voltage : latest.current;
    const float newValue = calibratedPreview(
        sensor, editor.measurement, inputFor(latest), editor.staged);
    char text[12];
    snprintf(text, sizeof(text), "%.1f", oldValue); lv_label_set_text(tab.oldValueLabel, text);
    snprintf(text, sizeof(text), "%.1f", newValue); lv_label_set_text(tab.newValueLabel, text);

    if (!appendPoint || !editor.preview || !editor.activeChart) return;

    // Preview points are append-only: a calibration adjustment takes effect
    // from the next sample onward and never redraws earlier history.
    const float scale = voltage ? kVoltageAxisScale : kCurrentAxisScale;
    bool appended = false;
    for (size_t i = 0; i < n; ++i) {
        if (readings[i].timestamp_ms <= editor.lastPreviewTimestamp) continue;
        const float preview = calibratedPreview(
            sensor, editor.measurement, inputFor(readings[i]), editor.staged);
        lv_chart_set_next_value(editor.activeChart, editor.preview, chartCoordinate(preview, scale));
        editor.lastPreviewTimestamp = readings[i].timestamp_ms;
        appended = true;
    }
    if (appended) lv_chart_refresh(editor.activeChart);
}

void refreshCalibrationEditor(SensorTab& tab, uint8_t sensor) {
    // This snapshot path is used by discrete calibration controls. The regular
    // Sensors timer already owns an equivalent buffer and passes it directly
    // to updateCalibrationEditor(), avoiding two 60-reading arrays on the
    // bounded LVGL task stack.
    sensors::Reading readings[kChartPoints];
    const size_t n = sensors::getRecent(
        static_cast<sensors::SensorId>(sensor), readings, kChartPoints);
    updateCalibrationEditor(tab, sensor, readings, n, false);
}

void updateRawCaptureViews() {
    for (uint8_t sensor = 0; sensor < sensors::SENSOR_COUNT; ++sensor) {
        SensorTab& tab = sensorTabs[sensor];
        if (!tab.contentRoot) continue;
        auto& capture = tab.capture;
        if (!capture.visible || !capture.ownsRequest) continue;

        const sensors::AdcCaptureStatus status = sensors::getAdcCaptureStatus();
        if (status.captureId != capture.requestId ||
            status.channel != static_cast<sensors::SensorId>(sensor)) {
            capture.ownsRequest = false;
            capture.requestId = 0;
            lv_label_set_text(capture.statusLabel, "Capture expired or was replaced");
            continue;
        }
        if (status.state == sensors::AdcCaptureState::Armed) {
            lv_label_set_text(capture.statusLabel, "Waiting for the next 500 ms window...");
            continue;
        }
        if (status.state == sensors::AdcCaptureState::Capturing) {
            char progress[64];
            snprintf(progress, sizeof(progress), "Capturing... %u/%u windows  %u samples",
                     status.windowCount, status.targetWindowCount, status.pointCount);
            lv_label_set_text(capture.statusLabel, progress);
            continue;
        }
        if (status.state == sensors::AdcCaptureState::Ready) {
            auto* result = static_cast<sensors::AdcCaptureResult*>(
                heap_policy::mallocPreferred(sizeof(sensors::AdcCaptureResult)));
            if (!result) {
                lv_label_set_text(capture.statusLabel, "Not enough memory to display capture");
                continue;
            }
            if (sensors::takeAdcCapture(capture.requestId, *result)) {
                showRawCaptureResult(tab, *result);
            } else {
                capture.ownsRequest = false;
                capture.requestId = 0;
                lv_label_set_text(capture.statusLabel, "Capture result unavailable");
            }
            heap_caps_free(result);
            continue;
        }

        capture.ownsRequest = false;
        capture.requestId = 0;
        lv_label_set_text(capture.statusLabel, "Capture stopped");
    }
}

void captureUpdateCb(lv_timer_t* timer) {
    if (timer && timer->user_data &&
        !lv_obj_is_visible(static_cast<lv_obj_t*>(timer->user_data))) return;
    // Keep capture formatting on a separate LVGL timer invocation so its
    // scratch buffers are unwound before the chart/calibration refresh runs.
    updateRawCaptureViews();
}

// ---------------------------------------------------------------------------
// Update loop
// ---------------------------------------------------------------------------

void updateCb(lv_timer_t* timer) {
    if (timer && timer->user_data && !lv_obj_is_visible(static_cast<lv_obj_t*>(timer->user_data))) return;
    sensors::Reading readings[kChartPoints];

    for (uint8_t i = 0; i < sensors::SENSOR_COUNT; i++) {
        SensorTab& tab = sensorTabs[i];
        if (!tab.contentRoot) continue;
        size_t n = sensors::getRecent(static_cast<sensors::SensorId>(i), readings, kChartPoints);
        if (n == 0) continue;

        const sensors::Reading& latest = readings[n - 1];
        if (rawCaptureAvailable() && sensors::isConfigured(latest)) {
            lv_obj_clear_flag(tab.captureButton, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(tab.captureButton, LV_OBJ_FLAG_HIDDEN);
        }

        size_t start = findStartAndAdvance(readings, n, tab.lastTimestamp, tab.primed);
        for (size_t k = start; k < n; k++) {
            lv_chart_set_next_value(tab.vChart, tab.vSeries,
                                     chartCoordinate(readings[k].voltage, kVoltageAxisScale));
            lv_chart_set_next_value(tab.iChart, tab.iSeries,
                                     chartCoordinate(readings[k].current, kCurrentAxisScale));
        }

        // --- Dynamic Y ranges, computed off the full visible window --------
        float vMin = kVoltageDefaultMin, vMax = kVoltageDefaultMax;
        float iMin = kCurrentDefaultMin, iMax = kCurrentDefaultMax;
        bool haveVoltage = false, haveCurrent = false;
        for (size_t k = 0; k < n; k++) {
            if (std::isfinite(readings[k].voltage)) {
                vMin = haveVoltage ? std::min(vMin, readings[k].voltage) : readings[k].voltage;
                vMax = haveVoltage ? std::max(vMax, readings[k].voltage) : readings[k].voltage;
                haveVoltage = true;
            }
            if (std::isfinite(readings[k].current)) {
                iMin = haveCurrent ? std::min(iMin, readings[k].current) : readings[k].current;
                iMax = haveCurrent ? std::max(iMax, readings[k].current) : readings[k].current;
                haveCurrent = true;
            }
        }

        float outMin, outMax;
        computeDynamicRange(vMin, vMax, kVoltageDefaultMin, kVoltageDefaultMax, 0.2f, tab.vRange, outMin, outMax);
        lv_chart_set_range(tab.vChart, LV_CHART_AXIS_PRIMARY_Y,
                            axisCoordinate(outMin, kVoltageAxisScale), axisCoordinate(outMax, kVoltageAxisScale));
        updateChartAxisLabels(tab.vFrame);

        computeDynamicRange(iMin, iMax, kCurrentDefaultMin, kCurrentDefaultMax, 0.3f, tab.iRange, outMin, outMax);
        lv_chart_set_range(tab.iChart, LV_CHART_AXIS_PRIMARY_Y,
                            axisCoordinate(outMin, kCurrentAxisScale), axisCoordinate(outMax, kCurrentAxisScale));
        updateChartAxisLabels(tab.iFrame);

        // --- KPI numbers: trailing 1s average, refreshed every 2s ----------
        bool dueForUpdate = !tab.kpiPrimed ||
                             (latest.timestamp_ms - tab.lastKpiTimestamp) >= kKpiUpdateIntervalMs;
        if (dueForUpdate) {
            float sumV = 0, sumI = 0, sumP = 0;
            size_t countV = 0, countI = 0, countP = 0;
            uint32_t windowStart = (latest.timestamp_ms > kKpiAverageWindowMs)
                                        ? (latest.timestamp_ms - kKpiAverageWindowMs)
                                        : 0;
            for (size_t k = n; k-- > 0;) {
                if (readings[k].timestamp_ms < windowStart) break;
                if (std::isfinite(readings[k].voltage)) { sumV += readings[k].voltage; ++countV; }
                if (std::isfinite(readings[k].current)) { sumI += readings[k].current; ++countI; }
                if (std::isfinite(readings[k].power)) { sumP += readings[k].power; ++countP; }
            }

            char buf[16];
            const bool observable = latest.state == sensors::ReadingState::Valid ||
                                    latest.state == sensors::ReadingState::OutOfRange;
            if (observable && countV) snprintf(buf, sizeof(buf), "%.1f", sumV / countV);
            else snprintf(buf, sizeof(buf), "--");
            lv_label_set_text(tab.vValueLabel, buf);
            if (observable && countI) snprintf(buf, sizeof(buf), "%.1f", sumI / countI);
            else snprintf(buf, sizeof(buf), "--");
            lv_label_set_text(tab.iValueLabel, buf);
            if (observable && countP) snprintf(buf, sizeof(buf), "%.0f", sumP / countP);
            else snprintf(buf, sizeof(buf), "--");
            lv_label_set_text(tab.pValueLabel, buf);
            const char* state = "";
            switch (latest.state) {
                case sensors::ReadingState::NotConfigured: state = "Not configured"; break;
                case sensors::ReadingState::Waiting: state = "Waiting"; break;
                case sensors::ReadingState::OutOfRange: state = "Out of range"; break;
                case sensors::ReadingState::Invalid: state = "Invalid"; break;
                case sensors::ReadingState::Stale: state = "Stale"; break;
                case sensors::ReadingState::Valid: break;
            }
            char status[32];
            if (state[0]) {
                snprintf(status, sizeof(status), LV_SYMBOL_WARNING " %s", state);
            } else {
                status[0] = '\0';
            }
            lv_label_set_text(tab.statusLabel, status);
            tab.lastKpiTimestamp = latest.timestamp_ms;
            tab.kpiPrimed = true;
        }

        // PWM builds reveal duty only after it has been observed below the
        // configured threshold. MPPT builds retain acquisition but omit it.
        if (hardware_profile::kControllerIsPwm) {
            float duty = sensors::getDutyCycle(static_cast<sensors::SensorId>(i));
            if (std::isfinite(duty) && !tab.dutyEverLow && duty < kDutyShowThreshold) {
                tab.dutyEverLow = true;
                lv_obj_clear_flag(tab.dutyRow, LV_OBJ_FLAG_HIDDEN);
            }
            if (tab.dutyEverLow) {
                char buf[8];
                if (std::isfinite(duty)) snprintf(buf, sizeof(buf), "%.0f", duty * 100.0f);
                else snprintf(buf, sizeof(buf), "--");
                lv_label_set_text(tab.dutyValueLabel, buf);
            }
        }

        if (tab.calibration.visible) updateCalibrationEditor(tab, i, readings, n, true);
    }

}

} // namespace

lv_obj_t* create(lv_obj_t* parent) {
    lv_obj_t* tabview = lv_tabview_create(parent, LV_DIR_TOP, 40);
    tabview_utils::disableSwipe(tabview);
    lv_obj_set_style_pad_all(tabview, 0, 0);
    lv_obj_set_style_border_width(tabview, 0, 0);
    lv_obj_set_style_radius(tabview, 0, 0);
    lv_obj_set_style_bg_color(tabview, ui_theme::background(), 0);
    lv_obj_set_style_bg_opa(tabview, LV_OPA_COVER, 0);
    // Leaving a sensor (or the Sensors top-level page) always abandons staged
    // calibration and returns to the normal live view on the next visit.
    lv_obj_add_event_cb(tabview, exitAllCalibrationCb, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_t* outerTabview = lv_obj_get_parent(lv_obj_get_parent(parent));
    if (outerTabview) lv_obj_add_event_cb(outerTabview, exitAllCalibrationCb, LV_EVENT_VALUE_CHANGED, nullptr);

    const char* names[sensors::SENSOR_COUNT] = {"Solar", "Load", "Battery"};
    for (uint8_t i = 0; i < sensors::SENSOR_COUNT; i++) {
        sensors::mapping::PhysicalSensorId physical;
        if (!sensors::mapping::physicalForLogical(
                sensor_mode::get(), static_cast<sensors::SensorId>(i), physical)) {
            continue;
        }
        lv_obj_t* tabBtn = lv_tabview_add_tab(tabview, names[i]);
        createSensorTab(tabBtn, i);
    }

    updateTimer = lv_timer_create(updateCb, kUpdateIntervalMs, tabview);
    captureUpdateTimer = lv_timer_create(captureUpdateCb, kUpdateIntervalMs, tabview);

    return tabview;
}

} // namespace sensors_screen
