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
static const float kCalibrationAxisScale = 1.0f;

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
    lv_obj_t* vChart = nullptr;
    lv_obj_t* iChart = nullptr;
    lv_chart_series_t* vSeries = nullptr;
    lv_chart_series_t* iSeries = nullptr;

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
        lv_obj_t* beforeLabel = nullptr;
        lv_obj_t* afterLabel = nullptr;
        lv_obj_t* offsetInput = nullptr;
        lv_obj_t* gainInput = nullptr;
        lv_obj_t* measurementInput = nullptr;
        lv_obj_t* keyboard = nullptr;
        lv_obj_t* chart = nullptr;
        lv_chart_series_t* before = nullptr;
        lv_chart_series_t* after = nullptr;
        sensors::calibration::Measurement measurement = sensors::calibration::Measurement::Voltage;
        sensors::calibration::Value saved{};
        sensors::calibration::Value staged{};
        uint8_t sensor = 0;
        bool visible = false;
    } calibration;
};

SensorTab sensorTabs[sensors::SENSOR_COUNT];

lv_timer_t* updateTimer = nullptr;

enum class CalibrationAction : uint8_t { Back, OffsetDown, OffsetUp, GainDown, GainUp, ApplyReference, Reset, Save };
struct CalibrationControl { SensorTab* tab; uint8_t sensor; CalibrationAction action; };
CalibrationControl calibrationControls[sensors::SENSOR_COUNT][8];

const char* measurementUnit(sensors::calibration::Measurement measurement) {
    return measurement == sensors::calibration::Measurement::Voltage ? "V" : "A";
}

void updateCalibrationEditor(SensorTab& tab, uint8_t sensor, const sensors::Reading* readings = nullptr, size_t n = 0);
void openVoltageCalibrationCb(lv_event_t* event);
void openCurrentCalibrationCb(lv_event_t* event);

void hideCalibrationKeyboard(lv_event_t* event) {
    auto* editor = static_cast<SensorTab::CalibrationEditor*>(lv_event_get_user_data(event));
    if (editor && editor->keyboard) {
        lv_obj_del(editor->keyboard);
        editor->keyboard = nullptr;
    }
}

void calibrationInputFocusCb(lv_event_t* event) {
    auto* editor = static_cast<SensorTab::CalibrationEditor*>(lv_event_get_user_data(event));
    if (!editor) return;
    if (!editor->keyboard) {
        editor->keyboard = lv_keyboard_create(editor->root);
        lv_obj_set_width(editor->keyboard, lv_pct(100));
        lv_obj_add_event_cb(editor->keyboard, hideCalibrationKeyboard, LV_EVENT_READY, editor);
        lv_obj_add_event_cb(editor->keyboard, hideCalibrationKeyboard, LV_EVENT_CANCEL, editor);
    }
    lv_keyboard_set_textarea(editor->keyboard, static_cast<lv_obj_t*>(lv_event_get_target(event)));
}

void refreshCalibrationInputs(SensorTab::CalibrationEditor& editor) {
    char text[20];
    snprintf(text, sizeof(text), "%.3f", editor.staged.offsetInputV);
    lv_textarea_set_text(editor.offsetInput, text);
    // The UI uses the inverse sensitivity: ADC millivolts per displayed
    // volt/amp. It keeps values close to the divider/sensor data sheet.
    snprintf(text, sizeof(text), "%.3f", 1000.0f / editor.staged.gain);
    lv_textarea_set_text(editor.gainInput, text);
}

void calibrationInputChangedCb(lv_event_t* event) {
    auto* editor = static_cast<SensorTab::CalibrationEditor*>(lv_event_get_user_data(event));
    if (!editor) return;
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

    if (control->action == CalibrationAction::Back) {
        editor.visible = false;
        // The keyboard/editor widgets use internal RAM. Keep only the active
        // editor alive, then release it fully on return to the live charts.
        lv_obj_del(editor.root);
        editor.root = nullptr;
        editor.keyboard = nullptr;
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
    if (control->action == CalibrationAction::ApplyReference) {
        const float reference = strtof(lv_textarea_get_text(editor.measurementInput), nullptr);
        const float max = editor.measurement == sensors::calibration::Measurement::Voltage
            ? sensors::calibration::kVoltageMaxV : sensors::calibration::kCurrentMaxA;
        sensors::Reading latest{};
        float input = NAN;
        if (sensors::getLatest(static_cast<sensors::SensorId>(sensor), latest)) {
            if (sensor_mode::get() == sensor_mode::Mode::Real) {
                input = editor.measurement == sensors::calibration::Measurement::Voltage ? latest.voltageInputV : latest.currentInputV;
            } else {
                const float displayed = editor.measurement == sensors::calibration::Measurement::Voltage ? latest.voltage : latest.current;
                input = displayed / editor.saved.gain + editor.saved.offsetInputV;
            }
        }
        const float denominator = input - editor.staged.offsetInputV;
        if (!std::isfinite(reference) || reference < 0.0f || reference > max || !std::isfinite(input) || fabsf(denominator) < 0.005f) {
            return;
        }
        editor.staged.gain = reference / denominator;
        if (!sensors::calibration::isValid(editor.measurement, editor.staged)) {
            return;
        }
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

// Draw-event callback that turns raw (scaled) tick values back into real
// units with one decimal place.
void axisTickLabelCb(lv_event_t* e) {
    lv_obj_draw_part_dsc_t* dsc = lv_event_get_draw_part_dsc(e);
    if (dsc == nullptr || dsc->part != LV_PART_TICKS || dsc->id != LV_CHART_AXIS_PRIMARY_Y) return;
    if (dsc->text == nullptr) return;
    const float* scale = static_cast<const float*>(lv_event_get_user_data(e));
    lv_snprintf(dsc->text, dsc->text_length, "%.1f", dsc->value / *scale);
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
                            const float* axisScale, lv_chart_series_t** seriesOut,
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

    if (title && title[0]) {
        lv_obj_t* titleRow = lv_obj_create(block);
        lv_obj_remove_style_all(titleRow);
        lv_obj_set_size(titleRow, lv_pct(100), 24);
        lv_obj_set_flex_flow(titleRow, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(titleRow, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        if (tab) {
            // The whole heading is deliberately a touch target. The cog is a
            // visual affordance, not a tiny control a finger must hit exactly.
            lv_obj_add_flag(titleRow, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(titleRow,
                measurement == sensors::calibration::Measurement::Voltage ? openVoltageCalibrationCb : openCurrentCalibrationCb,
                LV_EVENT_CLICKED, tab);
        }
        lv_obj_t* titleLabel = lv_label_create(titleRow);
        lv_label_set_text(titleLabel, title);
        lv_obj_set_style_text_font(titleLabel, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(titleLabel, ui_theme::mutedText(), 0);
        if (tab) {
            lv_obj_t* calibrate = lv_label_create(titleRow);
            lv_obj_set_size(calibrate, 42, 28);
            lv_obj_set_style_text_color(calibrate, ui_theme::mutedText(), 0);
            lv_label_set_text(calibrate, LV_SYMBOL_EDIT);
            lv_obj_add_flag(calibrate, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(calibrate,
                measurement == sensors::calibration::Measurement::Voltage ? openVoltageCalibrationCb : openCurrentCalibrationCb,
                LV_EVENT_CLICKED, tab);
        }
    }

    lv_obj_t* chart = lv_chart_create(block);
    lv_obj_set_size(chart, lv_pct(100), 0);
    lv_obj_set_flex_grow(chart, 1);
    lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
    lv_chart_set_update_mode(chart, LV_CHART_UPDATE_MODE_SHIFT);
    lv_chart_set_point_count(chart, kChartPoints);
    lv_chart_set_div_line_count(chart, 5, (uint8_t)kGridDivisions - 1);
    // Five major marks make the live scale legible at a glance (including
    // useful anchors such as 0, 5 and 10) without crowding the trace.
    lv_chart_set_axis_tick(chart, LV_CHART_AXIS_PRIMARY_Y, 4, 2, 5, 1, true, 52);
    lv_obj_set_style_clip_corner(chart, false, 0);

    styleChartMinimal(chart);
    lv_obj_set_style_pad_left(chart, 48, 0);
    lv_obj_add_event_cb(chart, axisTickLabelCb, LV_EVENT_DRAW_PART_BEGIN, (void*)axisScale);

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
    lv_obj_add_event_cb(button, calibrationControlCb, LV_EVENT_CLICKED, &control);
    return button;
}

void createCalibrationEditor(lv_obj_t* parent, SensorTab& tab, uint8_t sensor) {
    auto& editor = tab.calibration;
    editor.sensor = sensor;
    editor.keyboard = nullptr;
    editor.root = lv_obj_create(parent);
    ui_theme::styleScreen(editor.root, 4);
    lv_obj_set_flex_flow(editor.root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(editor.root, 4, 0);
    lv_obj_add_flag(editor.root, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t* header = lv_obj_create(editor.root);
    lv_obj_remove_style_all(header);
    lv_obj_set_size(header, lv_pct(100), 30);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(header, 6, 0);
    CalibrationControl& backControl = calibrationControls[sensor][0];
    backControl = {&tab, sensor, CalibrationAction::Back};
    lv_obj_t* back = lv_label_create(header);
    lv_label_set_text(back, LV_SYMBOL_LEFT);
    lv_obj_set_size(back, 36, 28);
    lv_obj_set_style_text_color(back, ui_theme::text(), 0);
    lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(back, calibrationControlCb, LV_EVENT_CLICKED, &backControl);
    lv_obj_t* title = lv_label_create(header);
    lv_obj_set_flex_grow(title, 1);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
    lv_label_set_text(title, "Calibration");

    lv_obj_t* kpis = lv_obj_create(editor.root);
    lv_obj_remove_style_all(kpis);
    lv_obj_set_size(kpis, lv_pct(100), 30);
    lv_obj_set_flex_flow(kpis, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(kpis, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    editor.beforeLabel = lv_label_create(kpis);
    editor.afterLabel = lv_label_create(kpis);
    lv_obj_set_style_text_font(editor.beforeLabel, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_font(editor.afterLabel, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(editor.beforeLabel, lv_palette_main(LV_PALETTE_BLUE), 0);
    lv_obj_set_style_text_color(editor.afterLabel, lv_palette_main(LV_PALETTE_ORANGE), 0);

    editor.chart = lv_chart_create(editor.root);
    lv_obj_set_size(editor.chart, lv_pct(100), 112);
    lv_chart_set_type(editor.chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(editor.chart, kChartPoints);
    lv_chart_set_update_mode(editor.chart, LV_CHART_UPDATE_MODE_CIRCULAR);
    lv_chart_set_div_line_count(editor.chart, 4, 3);
    lv_chart_set_axis_tick(editor.chart, LV_CHART_AXIS_PRIMARY_Y, 3, 1, 4, 1, true, 42);
    lv_obj_set_style_clip_corner(editor.chart, false, 0);
    styleChartMinimal(editor.chart);
    lv_obj_set_style_pad_left(editor.chart, 48, 0);
    lv_obj_add_event_cb(editor.chart, axisTickLabelCb, LV_EVENT_DRAW_PART_BEGIN, (void*)&kCalibrationAxisScale);
    editor.before = lv_chart_add_series(editor.chart, lv_palette_main(LV_PALETTE_BLUE), LV_CHART_AXIS_PRIMARY_Y);
    editor.after = lv_chart_add_series(editor.chart, lv_palette_main(LV_PALETTE_ORANGE), LV_CHART_AXIS_PRIMARY_Y);
    lv_obj_t* offsetTitle = lv_label_create(editor.root);
    lv_label_set_text(offsetTitle, "Offset");
    ui_theme::styleSectionLabel(offsetTitle);
    lv_obj_t* offsetRow = lv_obj_create(editor.root);
    lv_obj_remove_style_all(offsetRow);
    lv_obj_set_size(offsetRow, lv_pct(100), 32);
    lv_obj_set_flex_flow(offsetRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(offsetRow, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(offsetRow, 4, 0);
    createCalibrationButton(offsetRow, "-", calibrationControls[sensor][1], tab, sensor, CalibrationAction::OffsetDown);
    lv_obj_set_flex_grow(lv_obj_get_child(offsetRow, 0), 0); lv_obj_set_width(lv_obj_get_child(offsetRow, 0), 36);
    editor.offsetInput = lv_textarea_create(offsetRow);
    lv_obj_set_width(editor.offsetInput, 108); lv_textarea_set_one_line(editor.offsetInput, true);
    lv_obj_set_style_text_align(editor.offsetInput, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_event_cb(editor.offsetInput, calibrationInputFocusCb, LV_EVENT_FOCUSED, &editor);
    lv_obj_add_event_cb(editor.offsetInput, calibrationInputChangedCb, LV_EVENT_VALUE_CHANGED, &editor);
    lv_obj_t* offsetUnit = lv_label_create(offsetRow); lv_label_set_text(offsetUnit, "V"); lv_obj_set_width(offsetUnit, 18);
    lv_obj_set_style_text_align(offsetUnit, LV_TEXT_ALIGN_CENTER, 0);
    createCalibrationButton(offsetRow, "+", calibrationControls[sensor][2], tab, sensor, CalibrationAction::OffsetUp);
    lv_obj_set_flex_grow(lv_obj_get_child(offsetRow, 3), 0); lv_obj_set_width(lv_obj_get_child(offsetRow, 3), 36);

    lv_obj_t* gainTitle = lv_label_create(editor.root);
    lv_label_set_text(gainTitle, "Gain");
    ui_theme::styleSectionLabel(gainTitle);
    lv_obj_t* gainRow = lv_obj_create(editor.root);
    lv_obj_remove_style_all(gainRow);
    lv_obj_set_size(gainRow, lv_pct(100), 32);
    lv_obj_set_flex_flow(gainRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(gainRow, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(gainRow, 4, 0);
    createCalibrationButton(gainRow, "-", calibrationControls[sensor][3], tab, sensor, CalibrationAction::GainDown);
    lv_obj_set_flex_grow(lv_obj_get_child(gainRow, 0), 0); lv_obj_set_width(lv_obj_get_child(gainRow, 0), 36);
    editor.gainInput = lv_textarea_create(gainRow);
    lv_obj_set_width(editor.gainInput, 108); lv_textarea_set_one_line(editor.gainInput, true);
    lv_obj_set_style_text_align(editor.gainInput, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_event_cb(editor.gainInput, calibrationInputFocusCb, LV_EVENT_FOCUSED, &editor);
    lv_obj_add_event_cb(editor.gainInput, calibrationInputChangedCb, LV_EVENT_VALUE_CHANGED, &editor);
    lv_obj_t* gainUnit = lv_label_create(gainRow); lv_obj_set_width(gainUnit, 54);
    lv_label_set_text(gainUnit, "mV/V");
    lv_obj_set_style_text_align(gainUnit, LV_TEXT_ALIGN_CENTER, 0);
    createCalibrationButton(gainRow, "+", calibrationControls[sensor][4], tab, sensor, CalibrationAction::GainUp);
    lv_obj_set_flex_grow(lv_obj_get_child(gainRow, 3), 0); lv_obj_set_width(lv_obj_get_child(gainRow, 3), 36);

    lv_obj_t* referenceRow = lv_obj_create(editor.root);
    lv_obj_remove_style_all(referenceRow);
    lv_obj_set_size(referenceRow, lv_pct(100), 32);
    lv_obj_set_flex_flow(referenceRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(referenceRow, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(referenceRow, 4, 0);
    lv_obj_t* measurementTitle = lv_label_create(referenceRow); lv_label_set_text(measurementTitle, "Measurement");
    editor.measurementInput = lv_textarea_create(referenceRow);
    lv_obj_set_width(editor.measurementInput, 78); lv_textarea_set_one_line(editor.measurementInput, true); lv_textarea_set_text(editor.measurementInput, "0");
    lv_obj_set_style_text_align(editor.measurementInput, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_event_cb(editor.measurementInput, calibrationInputFocusCb, LV_EVENT_FOCUSED, &editor);
    lv_obj_t* measurementUnit = lv_label_create(referenceRow); lv_obj_set_width(measurementUnit, 14);
    createCalibrationButton(referenceRow, "Match", calibrationControls[sensor][5], tab, sensor, CalibrationAction::ApplyReference);

    lv_obj_t* actionRow = lv_obj_create(editor.root);
    lv_obj_remove_style_all(actionRow);
    lv_obj_set_size(actionRow, lv_pct(100), 32);
    lv_obj_set_flex_flow(actionRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(actionRow, 4, 0);
    createCalibrationButton(actionRow, "Defaults", calibrationControls[sensor][6], tab, sensor, CalibrationAction::Reset);
    createCalibrationButton(actionRow, "Save", calibrationControls[sensor][7], tab, sensor, CalibrationAction::Save, true);

}

void openCalibration(SensorTab& tab, uint8_t sensor, sensors::calibration::Measurement measurement) {
    auto& editor = tab.calibration;
    if (!editor.root) createCalibrationEditor(tab.calibrationParent, tab, sensor);
    editor.measurement = measurement;
    editor.saved = sensors::calibration::get(sensor, measurement);
    editor.staged = editor.saved;
    editor.visible = true;
    refreshCalibrationInputs(editor);
    // Units are tied to the selected engineering measurement, while gain is
    // shown as ADC millivolts per engineering unit.
    lv_label_set_text(lv_obj_get_child(lv_obj_get_parent(editor.gainInput), 2),
                      measurement == sensors::calibration::Measurement::Voltage ? "mV/V" : "mV/A");
    lv_label_set_text(lv_obj_get_child(lv_obj_get_parent(editor.measurementInput), 2), measurementUnit(measurement));
    lv_obj_clear_flag(editor.root, LV_OBJ_FLAG_HIDDEN);
    updateCalibrationEditor(tab, sensor);
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

    // --- KPI row -----------------------------------------------------------
    lv_obj_t* kpiRow = lv_obj_create(tab);
    styleFlatContainer(kpiRow);
    lv_obj_set_size(kpiRow, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_border_width(kpiRow, 0, 0);
    lv_obj_set_style_pad_all(kpiRow, 0, 0);
    lv_obj_set_style_pad_column(kpiRow, 14, 0);
    lv_obj_set_flex_flow(kpiRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(kpiRow, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    createKpiItem(kpiRow, "V", 54, &t.vValueLabel);
    createKpiItem(kpiRow, "A", 54, &t.iValueLabel);
    createKpiItem(kpiRow, "W", 48, &t.pValueLabel);
    lv_label_set_text(t.pValueLabel, "--");
    t.dutyRow = createKpiItem(kpiRow, "% duty", 40, &t.dutyValueLabel);
    lv_obj_add_flag(t.dutyRow, LV_OBJ_FLAG_HIDDEN); // hidden until it's seen below threshold

    // --- Voltage + current charts, stacked, sharing remaining space --------
    lv_obj_t* chartsCol = lv_obj_create(tab);
    styleFlatContainer(chartsCol);
    lv_obj_set_size(chartsCol, lv_pct(100), 0);
    lv_obj_set_style_border_width(chartsCol, 0, 0);
    lv_obj_set_style_pad_all(chartsCol, 0, 0);
    lv_obj_set_style_pad_row(chartsCol, 6, 0);
    lv_obj_set_flex_flow(chartsCol, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_grow(chartsCol, 1);

    t.vChart = createChartBlock(chartsCol, "Voltage (V)", lv_palette_main(LV_PALETTE_BLUE),
                                 &kVoltageAxisScale, &t.vSeries, &t, sensors::calibration::Measurement::Voltage);
    t.iChart = createChartBlock(chartsCol, "Current (A)", lv_palette_main(LV_PALETTE_ORANGE),
                                 &kCurrentAxisScale, &t.iSeries, &t, sensors::calibration::Measurement::Current);

    lv_chart_set_range(t.vChart, LV_CHART_AXIS_PRIMARY_Y,
                        (lv_coord_t)(kVoltageDefaultMin * kVoltageAxisScale),
                        (lv_coord_t)(kVoltageDefaultMax * kVoltageAxisScale));
    lv_chart_set_range(t.iChart, LV_CHART_AXIS_PRIMARY_Y,
                        (lv_coord_t)(kCurrentDefaultMin * kCurrentAxisScale),
                        (lv_coord_t)(kCurrentDefaultMax * kCurrentAxisScale));

    return tab;
}

void updateCalibrationEditor(SensorTab& tab, uint8_t sensor, const sensors::Reading* suppliedReadings, size_t suppliedCount) {
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
    const float input = inputFor(latest);
    const float savedReading = voltage ? latest.voltage : latest.current;
    const float preview = sensors::calibration::apply(input, editor.staged);
    const char* unit = measurementUnit(editor.measurement);
    char text[72];
    snprintf(text, sizeof(text), "Before %.1f %s", savedReading, unit);
    lv_label_set_text(editor.beforeLabel, text);
    snprintf(text, sizeof(text), "After %.1f %s", preview, unit);
    lv_label_set_text(editor.afterLabel, text);

    float low = savedReading;
    float high = savedReading;
    for (size_t i = 0; i < kChartPoints; ++i) {
        if (i < n) {
            const float before = voltage ? readings[i].voltage : readings[i].current;
            const float raw = inputFor(readings[i]);
            const float after = sensors::calibration::apply(raw, editor.staged);
            lv_chart_set_value_by_id(editor.chart, editor.before, i, lroundf(before));
            lv_chart_set_value_by_id(editor.chart, editor.after, i, lroundf(after));
            low = std::min(low, std::min(before, after));
            high = std::max(high, std::max(before, after));
        } else {
            lv_chart_set_value_by_id(editor.chart, editor.before, i, LV_CHART_POINT_NONE);
            lv_chart_set_value_by_id(editor.chart, editor.after, i, LV_CHART_POINT_NONE);
        }
    }
    const float maxAllowed = voltage ? sensors::calibration::kVoltageMaxV : sensors::calibration::kCurrentMaxA;
    low = std::max(-maxAllowed, low - 1.0f);
    high = std::min(maxAllowed, high + 1.0f);
    if (high <= low) high = low + 1.0f;
    lv_chart_set_range(editor.chart, LV_CHART_AXIS_PRIMARY_Y, lroundf(low), lroundf(high));
    lv_chart_refresh(editor.chart);
}

// ---------------------------------------------------------------------------
// Update loop
// ---------------------------------------------------------------------------

void updateCb(lv_timer_t*) {
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

        computeDynamicRange(iMin, iMax, kCurrentDefaultMin, kCurrentDefaultMax, 0.3f, tab.iRange, outMin, outMax);
        lv_chart_set_range(tab.iChart, LV_CHART_AXIS_PRIMARY_Y,
                            (lv_coord_t)(outMin * kCurrentAxisScale), (lv_coord_t)(outMax * kCurrentAxisScale));

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

        updateCalibrationEditor(tab, i, readings, n);
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

    const char* names[sensors::SENSOR_COUNT] = {"In", "Out", "Aux"};
    for (uint8_t i = 0; i < sensors::SENSOR_COUNT; i++) {
        lv_obj_t* tabBtn = lv_tabview_add_tab(tabview, names[i]);
        createSensorTab(tabBtn, i);
    }

    updateTimer = lv_timer_create(updateCb, kUpdateIntervalMs, nullptr);

    return tabview;
}

} // namespace sensors_screen
