#include "usage_screen.h"

#include "../../data/historical_storage.h"
#include "../../data/history_query_service.h"
#include "../../time/time_service.h"
#include "../components/linear_progress.h"
#include "../components/stacked_bar_chart.h"
#include "../theme/ui_theme.h"

#include <Arduino.h>
#include <cmath>
#include <cstdio>

namespace usage_screen {
namespace {

constexpr size_t kMaxPoints = 336; // 14 days * 24 one-hour buckets
constexpr uint32_t kRefreshIntervalMs = 60000;

struct Range {
    const char* title;
    uint32_t lookbackMinutes;
    uint16_t bucketMinutes;
    uint16_t tickMinutes;
    bool calendar;
    historical_storage::CalendarRange calendarRange;
};

constexpr Range kRanges[] = {
    {"Last 1 Hour", 60, 2, 15, false, historical_storage::CalendarRange::Today},
    {"Last 6 Hours", 360, 15, 60, false, historical_storage::CalendarRange::Today},
    {"Last 24 Hours", 1440, 30, 180, false, historical_storage::CalendarRange::Today},
    {"Today", 1440, 30, 180, true, historical_storage::CalendarRange::Today},
    {"Yesterday", 1440, 30, 180, true, historical_storage::CalendarRange::Yesterday},
    {"Last 2 Days", 2880, 60, 360, true, historical_storage::CalendarRange::Last2Days},
    {"Last Week", 10080, 240, 1440, true, historical_storage::CalendarRange::LastWeek},
    {"All", 0, 0, 0, true, historical_storage::CalendarRange::All},
};

lv_obj_t* chart = nullptr;
float chartValues[5][kMaxPoints] = {};
stacked_bar_chart::Series chartSeries[5] = {
    {lv_color_hex(0x159947), chartValues[0], true}, {lv_color_hex(0x0000FF), chartValues[1], true},
    {lv_color_hex(0x00BFFF), chartValues[2], true}, {lv_color_hex(0xFF4500), chartValues[3], false},
    {lv_color_hex(0xFFA500), chartValues[4], false},
};
lv_obj_t* rangeDropdown = nullptr;
lv_obj_t* statusBadge = nullptr;
lv_obj_t* statusIcon = nullptr;
lv_obj_t* statusText = nullptr;
lv_obj_t* progress = nullptr;
uint8_t selectedRange = 0;
lv_obj_t* screenObject = nullptr;
uint32_t pendingJob = 0;

lv_color_t seriesColor(uint8_t index)
{
    // Preserve the familiar energy semantics in both themes, while lifting
    // and softening the traces enough to remain comfortable on a dark panel.
    static constexpr uint32_t light[] = {0x159947, 0x0000FF, 0x00BFFF, 0xFF4500, 0xFFA500};
    static constexpr uint32_t dark[] = {0x3CA76C, 0x5596E6, 0x4DB6D0, 0xE56C63, 0xD99A58};
    return lv_color_hex(ui_theme::isDark() ? dark[index] : light[index]);
}

uint16_t automaticTickMinutes(uint32_t axisMinutes) {
    constexpr uint16_t kChoices[] = {15, 30, 60, 120, 240, 360, 720, 1440, 2880, 4320, 10080};
    const uint32_t target = (axisMinutes + 6) / 7; // roughly seven readable labels
    for (uint16_t choice : kChoices) if (choice >= target) return choice;
    return kChoices[sizeof(kChoices) / sizeof(kChoices[0]) - 1];
}

void setStatus(const char* text, bool warning = false) {
    if (!statusBadge || !statusIcon || !statusText) return;
    lv_label_set_text(statusText, text ? text : "");
    if (warning) {
        lv_label_set_text(statusIcon, LV_SYMBOL_WARNING);
        lv_obj_clear_flag(statusIcon, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(statusIcon, LV_OBJ_FLAG_HIDDEN);
    }
    if (text && text[0]) lv_obj_clear_flag(statusBadge, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(statusBadge, LV_OBJ_FLAG_HIDDEN);
}

void updateRangeOptions()
{
    char options[144] = {};
    size_t used = 0;
    for (uint8_t i = 0; i < sizeof(kRanges) / sizeof(kRanges[0]); ++i) {
        if (i && used + 1 < sizeof(options)) options[used++] = '\n';
        const int written = lv_snprintf(options + used, sizeof(options) - used, "%s", kRanges[i].title);
        if (written > 0) used += static_cast<size_t>(written);
    }
    lv_dropdown_set_options(rangeDropdown, options);
    lv_dropdown_set_selected(rangeDropdown, selectedRange);
}

void renderChart(const historical_storage::PowerBucket* buckets, size_t count,
                 const historical_storage::QueryStatus& status)
{
    const Range& range = kRanges[selectedRange];
    const uint32_t axisMinutes = status.endUnixMs > status.startUnixMs
        ? static_cast<uint32_t>((status.endUnixMs - status.startUnixMs + 59999) / 60000)
        : range.lookbackMinutes;
    const size_t expectedPoints = count;
    for (size_t i = 0; i < expectedPoints; ++i) for (auto& values : chartValues) values[i] = 0;

    for (size_t point = 0; point < expectedPoints; ++point) {
        const auto& bucket = buckets[point];
        const float charge = bucket.componentAveragePowerW[historical_storage::BATTERY_CHARGING];
        const float use = bucket.componentAveragePowerW[historical_storage::BATTERY_USAGE];
        const float panel = bucket.componentAveragePowerW[historical_storage::PANEL_IN];
        const float surplus = bucket.componentAveragePowerW[historical_storage::PANEL_SURPLUS];

        chartValues[0][point] = charge;
        chartValues[1][point] = panel;
        chartValues[2][point] = surplus;
        chartValues[3][point] = use;
        chartValues[4][point] = panel;
    }
    if (!time_service::hasCurrentTime()) setStatus("Set time to view history", true);
    else if (status.hasInferredTime) setStatus("some timestamps inferred", true);
    // Gaps are apparent in the chart. They are not a time-quality warning,
    // so anchored data never receives a bare warning icon.
    else if (!status.coveredMinutes) setStatus("No complete intervals yet");
    else setStatus("");
    const uint16_t tickMinutes = range.calendarRange == historical_storage::CalendarRange::All
        ? automaticTickMinutes(axisMinutes) : range.tickMinutes;
    stacked_bar_chart::setData(chart, {chartSeries, 5, expectedPoints, axisMinutes,
                                      tickMinutes, nullptr, status.startUnixMs,
                                      time_service::utcOffsetMinutes()});
}

void startQuery(bool replacePending = false)
{
    if (!screenObject || !lv_obj_is_visible(screenObject)) return;
    if (pendingJob && !replacePending) return;
    const Range& range = kRanges[selectedRange];
    pendingJob = history_query_service::requestUsage({range.calendar, range.calendarRange,
                                                       range.lookbackMinutes, range.bucketMinutes});
    if (!pendingJob) {
        setStatus("history service unavailable", true);
        return;
    }
    linear_progress::show(progress);
    // The progress bar is intentionally the only loading affordance. Keeping
    // this label out of layout lets the chart use the whole remaining page.
}

void completionCb(lv_timer_t*) {
    if (!pendingJob || !screenObject || !lv_obj_is_visible(screenObject)) return;
    static historical_storage::PowerBucket buckets[kMaxPoints];
    historical_storage::QueryStatus status{};
    size_t count = 0;
    history_query_service::Timing timing{};
    if (!history_query_service::takeUsage(pendingJob, buckets, kMaxPoints, count, status, &timing)) return;
    pendingJob = 0;
    linear_progress::hide(progress);
    renderChart(buckets, count, status);
}

void rangeChangedCb(lv_event_t* event)
{
    if (lv_event_get_code(event) == LV_EVENT_READY) {
        lv_obj_t* list = lv_dropdown_get_list(lv_event_get_target(event));
        lv_obj_set_style_text_font(list, &lv_font_montserrat_16, LV_PART_MAIN);
        lv_obj_set_style_text_line_space(list, 13, LV_PART_MAIN);
        lv_obj_set_style_pad_top(list, 4, LV_PART_MAIN);
        lv_obj_set_style_pad_bottom(list, 4, LV_PART_MAIN);
        return;
    }
    if (lv_event_get_code(event) != LV_EVENT_VALUE_CHANGED) return;
    const uint16_t selected = lv_dropdown_get_selected(lv_event_get_target(event));
    if (selected >= sizeof(kRanges) / sizeof(kRanges[0])) return;
    selectedRange = selected;
    startQuery(true);
}

void screenRefreshCb(lv_event_t* event) {
    // A query from this hidden screen may have been superseded by another
    // history request. Requeue on activation instead of retaining a job ID
    // that can no longer complete.
    if (lv_event_get_code(event) == LV_EVENT_REFRESH) startQuery(true);
}

void addLegendItem(lv_obj_t* parent, lv_color_t color, const char* text)
{
    lv_obj_t* label = lv_label_create(parent);
    lv_obj_set_style_text_color(label, color, 0);
    lv_label_set_text(label, text);
}

} // namespace

lv_obj_t* create(lv_obj_t* parent)
{
    lv_obj_t* screen = lv_obj_create(parent);
    screenObject = screen;
    // Usage is chart-first: compact controls leave the rest of the page for
    // the data rather than a visually empty footer.
    ui_theme::styleScreen(screen, 4);
    lv_obj_set_style_pad_row(screen, 4, 0);
    lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);

    rangeDropdown = lv_dropdown_create(screen);
    lv_obj_set_width(rangeDropdown, lv_pct(100));
    lv_obj_set_height(rangeDropdown, 42);
    lv_dropdown_set_symbol(rangeDropdown, LV_SYMBOL_DOWN);
    lv_dropdown_set_dir(rangeDropdown, LV_DIR_BOTTOM);
    lv_obj_set_style_text_font(rangeDropdown, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_border_width(rangeDropdown, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(rangeDropdown, rangeChangedCb, LV_EVENT_ALL, nullptr);

    lv_obj_t* legend = lv_obj_create(screen);
    lv_obj_remove_style_all(legend);
    lv_obj_set_size(legend, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(legend, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(legend, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    for (uint8_t i = 0; i < 5; ++i) chartSeries[i].color = seriesColor(i);
    addLegendItem(legend, seriesColor(0), "Charge");
    addLegendItem(legend, seriesColor(3), "Battery");
    addLegendItem(legend, seriesColor(1), "Panel");
    addLegendItem(legend, seriesColor(4), "Load");
    addLegendItem(legend, seriesColor(2), "Surplus");

    chart = stacked_bar_chart::create(screen);
    lv_obj_set_height(chart, 0);
    lv_obj_set_flex_grow(chart, 1);

    statusBadge = lv_obj_create(screen);
    lv_obj_remove_style_all(statusBadge);
    lv_obj_set_size(statusBadge, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(statusBadge, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(statusBadge, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_color(statusBadge, ui_theme::surface(), 0);
    lv_obj_set_style_bg_opa(statusBadge, LV_OPA_80, 0);
    lv_obj_set_style_pad_all(statusBadge, 2, 0);
    lv_obj_set_style_pad_column(statusBadge, 2, 0);
    lv_obj_set_style_radius(statusBadge, 3, 0);
    lv_obj_add_flag(statusBadge, LV_OBJ_FLAG_FLOATING | LV_OBJ_FLAG_IGNORE_LAYOUT | LV_OBJ_FLAG_HIDDEN);
    // Sit directly above the x-axis labels without consuming chart height.
    lv_obj_align(statusBadge, LV_ALIGN_BOTTOM_RIGHT, -2, -23);
    statusIcon = lv_label_create(statusBadge);
    lv_obj_set_style_text_color(statusIcon, lv_palette_main(LV_PALETTE_GREY), 0);
    lv_obj_set_style_text_font(statusIcon, &lv_font_montserrat_14, 0);
    statusText = lv_label_create(statusBadge);
    lv_obj_set_style_text_color(statusText, lv_palette_main(LV_PALETTE_GREY), 0);
    // 12 px is close to 80% of the regular 14 px UI text while remaining legible.
    lv_obj_set_style_text_font(statusText, &lv_font_montserrat_12, 0);
    progress = linear_progress::create(screen);
    lv_obj_add_event_cb(screen, screenRefreshCb, LV_EVENT_REFRESH, nullptr);

    updateRangeOptions();
    // This timer only transfers completed background results into LVGL; it
    // never performs filesystem work or aggregation.
    lv_timer_create(completionCb, 40, nullptr);
    return screen;
}

} // namespace usage_screen
