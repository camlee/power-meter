#include "sensors_screen.h"
#include "../../sensors/sensors.h"
#include "../theme/ui_theme.h"
#include <cstdio>
#include <cmath>
#include <algorithm>

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
};

SensorTab sensorTabs[sensors::SENSOR_COUNT];

lv_timer_t* updateTimer = nullptr;

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
                            const float* axisScale, lv_chart_series_t** seriesOut) {
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
        lv_obj_t* titleLabel = lv_label_create(block);
        lv_label_set_text(titleLabel, title);
        lv_obj_set_style_text_font(titleLabel, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(titleLabel, ui_theme::mutedText(), 0);
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
                                 &kVoltageAxisScale, &t.vSeries);
    t.iChart = createChartBlock(chartsCol, "Current (A)", lv_palette_main(LV_PALETTE_ORANGE),
                                 &kCurrentAxisScale, &t.iSeries);

    lv_chart_set_range(t.vChart, LV_CHART_AXIS_PRIMARY_Y,
                        (lv_coord_t)(kVoltageDefaultMin * kVoltageAxisScale),
                        (lv_coord_t)(kVoltageDefaultMax * kVoltageAxisScale));
    lv_chart_set_range(t.iChart, LV_CHART_AXIS_PRIMARY_Y,
                        (lv_coord_t)(kCurrentDefaultMin * kCurrentAxisScale),
                        (lv_coord_t)(kCurrentDefaultMax * kCurrentAxisScale));

    return tab;
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
