#include "sensors_screen.h"
#include "../../sensors/sensors.h"
#include "../../sensors/sensor_calibration.h"
#include "../../sensors/sensor_mode.h"
#include "../theme/ui_theme.h"
#include <cstdio>
#include <cmath>
#include <algorithm>
#include <cstdlib>

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

enum class CalibrationAction : uint8_t {
    Back, OffsetDown, OffsetUp, GainDown, GainUp, AutoZero, CalculateGain,
    Cancel, Save, Reset, ConfirmGain, CancelGain
};
struct CalibrationControl { SensorTab* tab; uint8_t sensor; CalibrationAction action; };
CalibrationControl calibrationControls[sensors::SENSOR_COUNT][12];

const char* measurementUnit(sensors::calibration::Measurement measurement) {
    return measurement == sensors::calibration::Measurement::Voltage ? "V" : "A";
}

void updateCalibrationEditor(SensorTab& tab, uint8_t sensor, const sensors::Reading* readings = nullptr,
                             size_t n = 0, bool appendPoint = false);
void refreshCalibrationInputs(SensorTab::CalibrationEditor& editor);
void openVoltageCalibrationCb(lv_event_t* event);
void openCurrentCalibrationCb(lv_event_t* event);
void closeCalibration(SensorTab& tab);

bool latestCalibrationInput(const SensorTab::CalibrationEditor& editor, float& input) {
    sensors::Reading latest{};
    if (!sensors::getLatest(static_cast<sensors::SensorId>(editor.sensor), latest)) return false;
    if (sensor_mode::get() == sensor_mode::Mode::Real) {
        input = editor.measurement == sensors::calibration::Measurement::Voltage
            ? latest.voltageInputV : latest.currentInputV;
    } else {
        const float displayed = editor.measurement == sensors::calibration::Measurement::Voltage
            ? latest.voltage : latest.current;
        input = displayed / editor.saved.gain + editor.saved.offsetInputV;
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
    const float denominator = input - editor.staged.offsetInputV;
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
    updateCalibrationEditor(tab, editor.sensor);
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
    updateCalibrationEditor(sensorTabs[editor->sensor], editor->sensor);
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
        editor.staged = sensors::calibration::defaults(editor.measurement);
    }
    if (control->action == CalibrationAction::Save) {
        if (sensor_mode::get() != sensor_mode::Mode::Real ||
            sensors::calibration::set(sensor, editor.measurement, editor.staged)) editor.saved = editor.staged;
    }
    refreshCalibrationInputs(editor);
    updateCalibrationEditor(tab, sensor);
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
    lv_obj_clear_flag(tab.kpiRow, LV_OBJ_FLAG_HIDDEN);
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
    for (SensorTab& tab : sensorTabs) closeCalibration(tab);
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
lv_obj_t* createChartBlock(lv_obj_t* parent, const char* title, lv_color_t color,
                            lv_obj_t** blockOut, lv_obj_t** editIconOut, SensorChartFrame* frame, const float* axisScale, lv_chart_series_t** seriesOut,
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
        lv_obj_set_style_text_font(titleLabel, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(titleLabel, ui_theme::mutedText(), 0);
        if (tab) {
            // Keep a finger-friendly target over the right quarter of the
            // heading while leaving the title itself non-interactive. The
            // glyph sits on the bottom edge, immediately above the plot.
            lv_obj_t* editTarget = lv_obj_create(titleRow);
            lv_obj_remove_style_all(editTarget);
            lv_obj_set_size(editTarget, lv_pct(25), lv_pct(100));
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
    lv_obj_set_size(item, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_border_width(item, 0, 0);
    lv_obj_set_style_pad_all(item, 0, 0);
    lv_obj_set_style_pad_column(item, 3, 0);
    lv_obj_set_flex_flow(item, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(item, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);

    lv_obj_t* valueLabel = lv_label_create(item);
    lv_label_set_text(valueLabel, "--.-");
    lv_obj_set_style_text_font(valueLabel, &lv_font_montserrat_20, 0);
    lv_obj_set_width(valueLabel, valueWidth);
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
    if (!editor.root) createCalibrationEditor(tab.chartsColumn, tab, sensor);
    editor.measurement = measurement;
    editor.saved = sensors::calibration::get(sensor, measurement);
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
    lv_obj_add_flag(tab.kpiRow, LV_OBJ_FLAG_HIDDEN);
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
        const float newValue = sensors::calibration::apply(
            sensor_mode::get() == sensor_mode::Mode::Real
                ? (voltage ? latest.voltageInputV : latest.currentInputV)
                : oldValue / editor.saved.gain + editor.saved.offsetInputV,
            editor.staged);
        char text[12];
        snprintf(text, sizeof(text), "%.1f", oldValue); lv_label_set_text(tab.oldValueLabel, text);
        snprintf(text, sizeof(text), "%.1f", newValue); lv_label_set_text(tab.newValueLabel, text);
    }
    lv_obj_clear_flag(editor.root, LV_OBJ_FLAG_HIDDEN);
}

void openVoltageCalibrationCb(lv_event_t* event) {
    auto* tab = static_cast<SensorTab*>(lv_event_get_user_data(event));
    if (!tab) return;
    const uint8_t sensor = static_cast<uint8_t>(tab - sensorTabs);
    openCalibration(*tab, sensor, sensors::calibration::Measurement::Voltage);
}

void openCurrentCalibrationCb(lv_event_t* event) {
    auto* tab = static_cast<SensorTab*>(lv_event_get_user_data(event));
    if (!tab) return;
    const uint8_t sensor = static_cast<uint8_t>(tab - sensorTabs);
    openCalibration(*tab, sensor, sensors::calibration::Measurement::Current);
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
    t.kpiRow = lv_obj_create(tab);
    styleFlatContainer(t.kpiRow);
    lv_obj_set_size(t.kpiRow, lv_pct(100), 30);
    lv_obj_set_style_border_width(t.kpiRow, 0, 0);
    lv_obj_set_style_pad_all(t.kpiRow, 0, 0);
    lv_obj_set_style_pad_column(t.kpiRow, 14, 0);
    lv_obj_set_flex_flow(t.kpiRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(t.kpiRow, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    createKpiItem(t.kpiRow, "V", 54, &t.vValueLabel);
    createKpiItem(t.kpiRow, "A", 54, &t.iValueLabel);
    createKpiItem(t.kpiRow, "W", 48, &t.pValueLabel);
    lv_label_set_text(t.pValueLabel, "--");
    t.dutyRow = createKpiItem(t.kpiRow, "%", 32, &t.dutyValueLabel);
    lv_obj_add_flag(t.dutyRow, LV_OBJ_FLAG_HIDDEN); // hidden until it's seen below threshold

    t.calibrationHeader = lv_obj_create(tab);
    lv_obj_remove_style_all(t.calibrationHeader);
    lv_obj_set_size(t.calibrationHeader, lv_pct(100), 30);
    lv_obj_set_flex_flow(t.calibrationHeader, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_left(t.calibrationHeader, 6, 0);
    lv_obj_set_style_pad_column(t.calibrationHeader, 3, 0);
    lv_obj_set_flex_align(t.calibrationHeader, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    CalibrationControl& backControl = calibrationControls[sensorIndex][0];
    backControl = {&t, sensorIndex, CalibrationAction::Back};
    lv_obj_t* back = lv_label_create(t.calibrationHeader);
    lv_label_set_text(back, LV_SYMBOL_LEFT);
    lv_obj_set_size(back, 28, 30);
    lv_obj_set_style_text_font(back, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_align(back, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(back, 4, 0);
    lv_obj_set_style_text_color(back, ui_theme::text(), 0);
    lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(back, calibrationControlCb, LV_EVENT_CLICKED, &backControl);
    lv_obj_t* title = lv_label_create(t.calibrationHeader);
    lv_obj_set_width(title, 91);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_label_set_text(title, "Calibration");
    t.oldLegend = lv_label_create(t.calibrationHeader);
    lv_label_set_text(t.oldLegend, "Old");
    lv_obj_set_width(t.oldLegend, 26);
    lv_label_set_long_mode(t.oldLegend, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_color(t.oldLegend, lv_palette_main(LV_PALETTE_BLUE), 0);
    lv_obj_set_style_text_font(t.oldLegend, &lv_font_montserrat_14, 0);
    t.oldValueLabel = lv_label_create(t.calibrationHeader);
    lv_obj_set_width(t.oldValueLabel, 32);
    lv_obj_set_style_text_align(t.oldValueLabel, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_color(t.oldValueLabel, lv_palette_main(LV_PALETTE_BLUE), 0);
    lv_obj_set_style_text_font(t.oldValueLabel, &lv_font_montserrat_14, 0);
    lv_label_set_text(t.oldValueLabel, "--.-");
    t.oldUnitLabel = lv_label_create(t.calibrationHeader);
    lv_obj_set_width(t.oldUnitLabel, 12);
    lv_obj_set_style_text_color(t.oldUnitLabel, lv_palette_main(LV_PALETTE_BLUE), 0);
    lv_obj_set_style_text_font(t.oldUnitLabel, &lv_font_montserrat_14, 0);
    lv_label_set_text(t.oldUnitLabel, "V");
    t.newLegend = lv_label_create(t.calibrationHeader);
    lv_label_set_text(t.newLegend, "New");
    lv_obj_set_width(t.newLegend, 34);
    lv_label_set_long_mode(t.newLegend, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_color(t.newLegend, lv_palette_main(LV_PALETTE_ORANGE), 0);
    lv_obj_set_style_text_font(t.newLegend, &lv_font_montserrat_14, 0);
    t.newValueLabel = lv_label_create(t.calibrationHeader);
    lv_obj_set_width(t.newValueLabel, 32);
    lv_obj_set_style_text_align(t.newValueLabel, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_color(t.newValueLabel, lv_palette_main(LV_PALETTE_ORANGE), 0);
    lv_obj_set_style_text_font(t.newValueLabel, &lv_font_montserrat_14, 0);
    lv_label_set_text(t.newValueLabel, "--.-");
    t.newValueInput = lv_textarea_create(t.calibrationHeader);
    styleCalibrationInput(t.newValueInput);
    lv_obj_set_size(t.newValueInput, 47, 28);
    lv_obj_set_style_text_font(t.newValueInput, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(t.newValueInput, lv_palette_main(LV_PALETTE_ORANGE), 0);
    lv_obj_set_style_border_color(t.newValueInput, lv_palette_main(LV_PALETTE_ORANGE), 0);
    lv_obj_set_style_border_color(t.newValueInput, lv_palette_main(LV_PALETTE_ORANGE), LV_STATE_FOCUSED);
    lv_obj_set_style_pad_top(t.newValueInput, 4, 0);
    lv_obj_set_style_pad_bottom(t.newValueInput, 3, 0);
    lv_obj_add_flag(t.newValueInput, LV_OBJ_FLAG_HIDDEN);
    t.newUnitLabel = lv_label_create(t.calibrationHeader);
    lv_obj_set_width(t.newUnitLabel, 12);
    lv_obj_set_style_text_color(t.newUnitLabel, lv_palette_main(LV_PALETTE_ORANGE), 0);
    lv_obj_set_style_text_font(t.newUnitLabel, &lv_font_montserrat_14, 0);
    lv_label_set_text(t.newUnitLabel, "V");
    lv_obj_add_flag(t.calibrationHeader, LV_OBJ_FLAG_HIDDEN);

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
    t.vChart = createChartBlock(t.chartsColumn, "Voltage (V)", lv_palette_main(LV_PALETTE_BLUE),
                                 &t.vBlock, &t.vEditIcon, &t.vFrame, &kVoltageAxisScale, &t.vSeries, &t, sensors::calibration::Measurement::Voltage);
    t.iChart = createChartBlock(t.chartsColumn, "Current (A)", lv_palette_main(LV_PALETTE_ORANGE),
                                 &t.iBlock, &t.iEditIcon, &t.iFrame, &kCurrentAxisScale, &t.iSeries, &t, sensors::calibration::Measurement::Current);
    t.vPreviewSeries = lv_chart_add_series(t.vChart, lv_palette_main(LV_PALETTE_ORANGE), LV_CHART_AXIS_PRIMARY_Y);
    t.iPreviewSeries = lv_chart_add_series(t.iChart, lv_palette_main(LV_PALETTE_ORANGE), LV_CHART_AXIS_PRIMARY_Y);
    lv_chart_hide_series(t.vChart, t.vPreviewSeries, true);
    lv_chart_hide_series(t.iChart, t.iPreviewSeries, true);

    lv_chart_set_range(t.vChart, LV_CHART_AXIS_PRIMARY_Y,
                        (lv_coord_t)(kVoltageDefaultMin * kVoltageAxisScale),
                        (lv_coord_t)(kVoltageDefaultMax * kVoltageAxisScale));
    lv_chart_set_range(t.iChart, LV_CHART_AXIS_PRIMARY_Y,
                        (lv_coord_t)(kCurrentDefaultMin * kCurrentAxisScale),
                        (lv_coord_t)(kCurrentDefaultMax * kCurrentAxisScale));

    return tab;
}

void updateCalibrationEditor(SensorTab& tab, uint8_t sensor, const sensors::Reading* suppliedReadings,
                             size_t suppliedCount, bool appendPoint) {
    auto& editor = tab.calibration;
    if (!editor.visible) return;

    sensors::Reading local[kChartPoints];
    const sensors::Reading* readings = suppliedReadings;
    size_t n = suppliedCount;
    if (!readings) {
        n = sensors::getRecent(static_cast<sensors::SensorId>(sensor), local, kChartPoints);
        readings = local;
    }
    if (n == 0) return;

    const bool voltage = editor.measurement == sensors::calibration::Measurement::Voltage;
    const auto inputFor = [&](const sensors::Reading& reading) {
        if (sensor_mode::get() == sensor_mode::Mode::Real) return voltage ? reading.voltageInputV : reading.currentInputV;
        // Simulation supplies engineering units, not ADC volts. Reconstruct a
        // compatible input so the preview graph remains meaningful for UI
        // walkthroughs without ever applying/saving demo calibration.
        const float value = voltage ? reading.voltage : reading.current;
        return value / editor.saved.gain + editor.saved.offsetInputV;
    };
    const sensors::Reading& latest = readings[n - 1];
    const float oldValue = voltage ? latest.voltage : latest.current;
    const float newValue = sensors::calibration::apply(inputFor(latest), editor.staged);
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
        const float preview = sensors::calibration::apply(inputFor(readings[i]), editor.staged);
        lv_chart_set_next_value(editor.activeChart, editor.preview, lroundf(preview * scale));
        editor.lastPreviewTimestamp = readings[i].timestamp_ms;
        appended = true;
    }
    if (appended) lv_chart_refresh(editor.activeChart);
}

// ---------------------------------------------------------------------------
// Update loop
// ---------------------------------------------------------------------------

void updateCb(lv_timer_t* timer) {
    if (timer && timer->user_data && !lv_obj_is_visible(static_cast<lv_obj_t*>(timer->user_data))) return;
    sensors::Reading readings[kChartPoints];

    for (uint8_t i = 0; i < sensors::SENSOR_COUNT; i++) {
        size_t n = sensors::getRecent(static_cast<sensors::SensorId>(i), readings, kChartPoints);
        if (n == 0) continue;

        SensorTab& tab = sensorTabs[i];
        const sensors::Reading& latest = readings[n - 1];

        size_t start = findStartAndAdvance(readings, n, tab.lastTimestamp, tab.primed);
        for (size_t k = start; k < n; k++) {
            lv_chart_set_next_value(tab.vChart, tab.vSeries,
                                     (lv_coord_t)lroundf(readings[k].voltage * kVoltageAxisScale));
            lv_chart_set_next_value(tab.iChart, tab.iSeries,
                                     (lv_coord_t)lroundf(readings[k].current * kCurrentAxisScale));
        }

        // --- Dynamic Y ranges, computed off the full visible window --------
        float vMin = readings[0].voltage, vMax = readings[0].voltage;
        float iMin = readings[0].current, iMax = readings[0].current;
        for (size_t k = 1; k < n; k++) {
            vMin = std::min(vMin, readings[k].voltage);
            vMax = std::max(vMax, readings[k].voltage);
            iMin = std::min(iMin, readings[k].current);
            iMax = std::max(iMax, readings[k].current);
        }

        float outMin, outMax;
        computeDynamicRange(vMin, vMax, kVoltageDefaultMin, kVoltageDefaultMax, 0.2f, tab.vRange, outMin, outMax);
        lv_chart_set_range(tab.vChart, LV_CHART_AXIS_PRIMARY_Y,
                            (lv_coord_t)(outMin * kVoltageAxisScale), (lv_coord_t)(outMax * kVoltageAxisScale));
        updateChartAxisLabels(tab.vFrame);

        computeDynamicRange(iMin, iMax, kCurrentDefaultMin, kCurrentDefaultMax, 0.3f, tab.iRange, outMin, outMax);
        lv_chart_set_range(tab.iChart, LV_CHART_AXIS_PRIMARY_Y,
                            (lv_coord_t)(outMin * kCurrentAxisScale), (lv_coord_t)(outMax * kCurrentAxisScale));
        updateChartAxisLabels(tab.iFrame);

        // --- KPI numbers: trailing 1s average, refreshed every 2s ----------
        bool dueForUpdate = !tab.kpiPrimed ||
                             (latest.timestamp_ms - tab.lastKpiTimestamp) >= kKpiUpdateIntervalMs;
        if (dueForUpdate) {
            float sumV = 0, sumI = 0, sumP = 0;
            size_t count = 0;
            uint32_t windowStart = (latest.timestamp_ms > kKpiAverageWindowMs)
                                        ? (latest.timestamp_ms - kKpiAverageWindowMs)
                                        : 0;
            for (size_t k = n; k-- > 0;) {
                if (readings[k].timestamp_ms < windowStart) break;
                sumV += readings[k].voltage;
                sumI += readings[k].current;
                sumP += readings[k].power;
                count++;
            }
            if (count == 0) { sumV = latest.voltage; sumI = latest.current; sumP = latest.power; count = 1; }

            char buf[16];
            snprintf(buf, sizeof(buf), "%.1f", sumV / count);
            lv_label_set_text(tab.vValueLabel, buf);
            snprintf(buf, sizeof(buf), "%.1f", sumI / count);
            lv_label_set_text(tab.iValueLabel, buf);
            snprintf(buf, sizeof(buf), "%.0f", sumP / count);
            lv_label_set_text(tab.pValueLabel, buf);
            tab.lastKpiTimestamp = latest.timestamp_ms;
            tab.kpiPrimed = true;
        }

        // --- Duty: stays hidden until first seen below threshold, then always shown ---
        float duty = sensors::getDutyCycle(static_cast<sensors::SensorId>(i));
        if (!tab.dutyEverLow && duty < kDutyShowThreshold) {
            tab.dutyEverLow = true;
            lv_obj_clear_flag(tab.dutyRow, LV_OBJ_FLAG_HIDDEN);
        }
        if (tab.dutyEverLow) {
            char buf[8];
            snprintf(buf, sizeof(buf), "%.0f", duty * 100.0f);
            lv_label_set_text(tab.dutyValueLabel, buf);
        }

        if (tab.calibration.visible) updateCalibrationEditor(tab, i, readings, n, true);
    }

}

} // namespace

lv_obj_t* create(lv_obj_t* parent) {
    lv_obj_t* tabview = lv_tabview_create(parent, LV_DIR_TOP, 40);
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

    const char* names[sensors::SENSOR_COUNT] = {"In", "Out", "Aux"};
    for (uint8_t i = 0; i < sensors::SENSOR_COUNT; i++) {
        lv_obj_t* tabBtn = lv_tabview_add_tab(tabview, names[i]);
        createSensorTab(tabBtn, i);
    }

    updateTimer = lv_timer_create(updateCb, kUpdateIntervalMs, tabview);

    return tabview;
}

} // namespace sensors_screen
