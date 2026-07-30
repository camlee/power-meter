#include "usage_screen.h"

#include "../../data/historical_storage.h"
#include "../../data/history_query_service.h"
#include "../../data/power_flow.h"
#include "../../device/hardware_profile.h"
#include "../../memory/heap_policy.h"
#include "../../sensors/sensor_mapping.h"
#include "../../sensors/sensors.h"
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
constexpr uint32_t kRefreshCheckMs = 1000;
constexpr uint32_t kStorageBoundaryGraceMs = 2000;
constexpr uint32_t kQueryRetryMs = 30000;

struct Range {
    const char* title;
    history_query_service::UsageQueryKind kind;
    uint32_t lookbackMinutes;
    uint16_t bucketMinutes;
    uint16_t tickMinutes;
    historical_storage::CalendarRange calendarRange;
};

constexpr Range kRanges[] = {
    {"Last 1 Hour", history_query_service::UsageQueryKind::Rolling,
     60, 1, 15, historical_storage::CalendarRange::Today},
    {"Last 6 Hours", history_query_service::UsageQueryKind::Rolling,
     360, 10, 60, historical_storage::CalendarRange::Today},
    {"Last 24 Hours", history_query_service::UsageQueryKind::Rolling,
     1440, 30, 180, historical_storage::CalendarRange::Today},
    {"Last 2 Days", history_query_service::UsageQueryKind::Rolling,
     2880, 60, 360, historical_storage::CalendarRange::Today},
    {"Last Week", history_query_service::UsageQueryKind::Rolling,
     10080, 240, 1440, historical_storage::CalendarRange::Today},
    {"Today", history_query_service::UsageQueryKind::Calendar,
     0, 30, 180, historical_storage::CalendarRange::Today},
    {"Yesterday", history_query_service::UsageQueryKind::Calendar,
     0, 30, 180, historical_storage::CalendarRange::Yesterday},
    {"All History", history_query_service::UsageQueryKind::Calendar,
     0, 0, 0, historical_storage::CalendarRange::All},
    {"Since Boot", history_query_service::UsageQueryKind::SinceBoot,
     0, 0, 0, historical_storage::CalendarRange::Today},
};
constexpr uint8_t kRollingRangeCount = 5;
constexpr uint8_t kTodayRange = 5;
constexpr uint8_t kAllHistoryRange = 7;
constexpr uint8_t kSinceBootRange = 8;
constexpr uint8_t kDefaultRange = 0;

lv_obj_t* chart = nullptr;
struct MpptStorage {
    float solar[kMaxPoints];
    float load[kMaxPoints];
    float battery[kMaxPoints];
    bool batteryMeasured[kMaxPoints];
};
union ChartStorage {
    float stacked[5][kMaxPoints];
    MpptStorage mppt;
};
static_assert(
    sizeof(ChartStorage) <= sizeof(float) * 6 * kMaxPoints,
    "Usage chart storage must not exceed the previous six-series footprint");
ChartStorage* chartStorage = nullptr;
stacked_bar_chart::Series chartSeries[5] = {
    {lv_color_hex(0x159947), nullptr, true},
    {lv_color_hex(0x0000FF), nullptr, true},
    {lv_color_hex(0xFFA500), nullptr, false},
    {lv_color_hex(0xFF4500), nullptr, false},
    {lv_color_hex(0x00BFFF), nullptr, true},
};
stacked_bar_chart::RangeSeries rangeSeries[5] = {
    {lv_color_hex(0x159947), nullptr, nullptr},
    {lv_color_hex(0x0000FF), nullptr, nullptr},
    {lv_color_hex(0xFFA500), nullptr, nullptr},
    {lv_color_hex(0xFF4500), nullptr, nullptr},
    {lv_color_hex(0x808080), nullptr, nullptr},
};
lv_obj_t* rangeDropdown = nullptr;
lv_obj_t* balanceLegendItem = nullptr;
lv_obj_t* statusBadge = nullptr;
lv_obj_t* statusIcon = nullptr;
lv_obj_t* statusText = nullptr;
lv_obj_t* progress = nullptr;
uint8_t selectedRange = kDefaultRange;
uint8_t visibleRanges[sizeof(kRanges) / sizeof(kRanges[0])] = {};
uint8_t visibleRangeCount = 0;
bool rangeOptionsHaveTime = false;
bool showBalance = false;
lv_obj_t* screenObject = nullptr;
uint32_t pendingJob = 0;
uint32_t nextRefreshAtMs = 0;
uint16_t renderedBucketMinutes = 0;

bool ensureChartStorage() {
    if (chartStorage) return true;
    // The VIEWE build places this long-lived buffer in PSRAM. callocPreferred
    // retains an internal-RAM fallback for any future touch target without
    // PSRAM; today's WROOM build excludes the entire ui/ tree.
    chartStorage = static_cast<ChartStorage*>(
        heap_policy::callocPreferred(1, sizeof(ChartStorage)));
    if (!chartStorage) {
        Serial.println("usage: chart storage allocation failed");
        return false;
    }
    for (uint8_t series = 0; series < 5; ++series) {
        chartSeries[series].values = chartStorage->stacked[series];
    }
    return true;
}

bool provideMpptRanges(const void* context, size_t point,
                       stacked_bar_chart::RangeValue* values,
                       uint8_t valueCount) {
    if (!context || !values || point >= kMaxPoints || valueCount < 5) return false;
    const auto& storage = *static_cast<const MpptStorage*>(context);
    const power_flow::UsageBreakdown flow = power_flow::usage(
        storage.solar[point], storage.load[point], storage.battery[point],
        storage.batteryMeasured[point], showBalance);
    const power_flow::SegmentRange segments[] = {
        flow.chargeSegment,
        flow.solarSegment,
        flow.loadSegment,
        flow.dischargeSegment,
        flow.balanceSegment,
    };
    for (uint8_t series = 0; series < 5; ++series) {
        values[series] = {segments[series].from, segments[series].to};
    }
    return true;
}

lv_color_t seriesColor(uint8_t index)
{
    // Preserve the familiar energy semantics in both themes, while lifting
    // and softening the traces enough to remain comfortable on a dark panel.
    static constexpr uint32_t light[] = {0x159947, 0x0000FF, 0x00BFFF, 0xFF4500, 0xFFA500};
    static constexpr uint32_t dark[] = {0x3CA76C, 0x5596E6, 0x4DB6D0, 0xE56C63, 0xD99A58};
    return lv_color_hex(ui_theme::isDark() ? dark[index] : light[index]);
}

lv_color_t balanceColor() {
    return lv_color_hex(ui_theme::isDark() ? 0x7F8B92 : 0x8A949A);
}

float channelAveragePower(const historical_storage::PowerBucket& bucket, uint8_t channel) {
    if (channel >= historical_storage::kSensorCount || !bucket.channelCoverageMs[channel]) return NAN;
    return bucket.energyWh[channel] * 3600000.0f / bucket.channelCoverageMs[channel];
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

void updateRangeOptions(bool hasTime)
{
    if (hasTime && selectedRange == kSinceBootRange) selectedRange = kTodayRange;
    if (!hasTime && selectedRange >= kTodayRange && selectedRange <= kAllHistoryRange) {
        selectedRange = kSinceBootRange;
    }
    char options[144] = {};
    size_t used = 0;
    visibleRangeCount = 0;
    for (uint8_t i = 0; i < sizeof(kRanges) / sizeof(kRanges[0]); ++i) {
        const bool visible = i < kRollingRangeCount ||
                             (hasTime ? i >= kTodayRange && i <= kAllHistoryRange
                                      : i == kSinceBootRange);
        if (!visible) continue;
        if (visibleRangeCount && used + 1 < sizeof(options)) options[used++] = '\n';
        const int written = lv_snprintf(options + used, sizeof(options) - used, "%s", kRanges[i].title);
        if (written > 0) used += static_cast<size_t>(written);
        visibleRanges[visibleRangeCount++] = i;
    }
    lv_dropdown_set_options(rangeDropdown, options);
    uint8_t visibleSelection = 0;
    while (visibleSelection + 1 < visibleRangeCount &&
           visibleRanges[visibleSelection] != selectedRange) ++visibleSelection;
    lv_dropdown_set_selected(rangeDropdown, visibleSelection);
    rangeOptionsHaveTime = hasTime;
}

void renderChart(const historical_storage::PowerBucket* buckets, size_t count,
                 const historical_storage::QueryStatus& status)
{
    if (!chartStorage) return;
    const Range& range = kRanges[selectedRange];
    const uint32_t axisMinutes = status.endTimeMs > status.startTimeMs
        ? static_cast<uint32_t>((status.endTimeMs - status.startTimeMs + 59999) / 60000)
        : range.lookbackMinutes;
    const size_t expectedPoints = std::min(count, kMaxPoints);
    renderedBucketMinutes = count ? buckets[0].durationMinutes : range.bucketMinutes;

    bool hasMeasuredBattery = false;
    for (size_t point = 0; point < expectedPoints; ++point) {
        const auto& bucket = buckets[point];
        if (hardware_profile::kControllerIsPwm) {
            chartStorage->stacked[0][point] =
                bucket.componentAveragePowerW[historical_storage::BATTERY_CHARGING];
            chartStorage->stacked[1][point] =
                bucket.componentAveragePowerW[historical_storage::PANEL_IN];
            chartStorage->stacked[2][point] =
                bucket.componentAveragePowerW[historical_storage::PANEL_SURPLUS];
            chartStorage->stacked[3][point] =
                bucket.componentAveragePowerW[historical_storage::BATTERY_USAGE];
            chartStorage->stacked[4][point] =
                bucket.componentAveragePowerW[historical_storage::PANEL_USAGE];
        } else {
            const float solar = channelAveragePower(bucket, sensors::SENSOR_SOLAR);
            const float load = channelAveragePower(bucket, sensors::SENSOR_LOAD);
            const float directBattery =
                channelAveragePower(bucket, sensors::SENSOR_BATTERY);
            const bool batteryMeasured = std::isfinite(directBattery);
            hasMeasuredBattery = hasMeasuredBattery || batteryMeasured;
            float battery = directBattery;
            if (!batteryMeasured) {
                const float charge = bucket.componentAveragePowerW[
                    historical_storage::BATTERY_CHARGING];
                const float discharge = bucket.componentAveragePowerW[
                    historical_storage::BATTERY_USAGE];
                if (std::isfinite(charge) && std::isfinite(discharge)) {
                    battery = charge - discharge;
                }
            }
            chartStorage->mppt.solar[point] = solar;
            chartStorage->mppt.load[point] = load;
            chartStorage->mppt.battery[point] = battery;
            chartStorage->mppt.batteryMeasured[point] = batteryMeasured;
        }
    }
    if (balanceLegendItem) {
        if (showBalance && hasMeasuredBattery) {
            lv_obj_clear_flag(balanceLegendItem, LV_OBJ_FLAG_HIDDEN);
        }
        else lv_obj_add_flag(balanceLegendItem, LV_OBJ_FLAG_HIDDEN);
    }
    const bool relative =
        status.timelineBasis == historical_storage::TimelineBasis::CurrentSessionMonotonic;
    if (status.hasInferredTime) setStatus("some timestamps inferred", true);
    else if (status.incomplete) setStatus("sensor data gaps", true);
    // Keep time inference and measurement gaps explicit rather than rendering
    // missing coverage as a zero-height observation.
    else if (!status.coveredMinutes) setStatus("No complete intervals yet");
    else setStatus("");
    const uint16_t tickMinutes = range.tickMinutes
        ? range.tickMinutes : automaticTickMinutes(axisMinutes);
    stacked_bar_chart::Data data{};
    data.series = chartSeries;
    data.seriesCount = hardware_profile::kControllerIsPwm ? 5 : 0;
    data.pointCount = expectedPoints;
    data.durationMinutes = axisMinutes;
    data.tickMinutes = tickMinutes;
    data.axisMode = relative ? stacked_bar_chart::AxisMode::Relative
                             : stacked_bar_chart::AxisMode::WallClock;
    data.axisStartTimeMs = status.startTimeMs;
    data.utcOffsetMinutes = time_service::utcOffsetMinutes();
    if (!hardware_profile::kControllerIsPwm) {
        data.rangeSeries = rangeSeries;
        data.rangeSeriesCount = showBalance ? 5 : 4;
        data.rangePointProvider = provideMpptRanges;
        data.rangePointContext = &chartStorage->mppt;
    }
    stacked_bar_chart::setData(chart, data);
}

uint16_t refreshMinutes() {
    const Range& range = kRanges[selectedRange];
    if (range.kind == history_query_service::UsageQueryKind::Calendar &&
        range.calendarRange == historical_storage::CalendarRange::Yesterday) {
        return 0;
    }
    return range.bucketMinutes ? range.bucketMinutes : renderedBucketMinutes;
}

void scheduleNextRefresh() {
    const uint16_t minutes = refreshMinutes();
    if (!minutes) {
        nextRefreshAtMs = 0;
        return;
    }
    const uint32_t cadenceMs = static_cast<uint32_t>(minutes) * 60000UL;
    const uint32_t now = millis();
    // History rows close on monotonic minute boundaries. Query just after the
    // corresponding bucket boundary so repeated refreshes stay synchronized
    // with storage instead of drifting from the moment this screen opened.
    nextRefreshAtMs = now + (cadenceMs - now % cadenceMs) + kStorageBoundaryGraceMs;
}

void startQuery(bool replacePending = false)
{
    if (!screenObject || !lv_obj_is_visible(screenObject)) return;
    if (pendingJob) {
        if (!replacePending) return;
        history_query_service::cancel(pendingJob);
        pendingJob = 0;
    }
    const Range& range = kRanges[selectedRange];
    pendingJob = history_query_service::requestUsage({range.kind, range.calendarRange,
                                                       range.lookbackMinutes, range.bucketMinutes});
    if (!pendingJob) {
        setStatus("history service unavailable", true);
        nextRefreshAtMs = millis() + kQueryRetryMs;
        return;
    }
    linear_progress::show(progress);
    // The progress bar is intentionally the only loading affordance. Keeping
    // this label out of layout lets the chart use the whole remaining page.
}

void completionCb(lv_timer_t*) {
    if (!pendingJob || !screenObject || !lv_obj_is_visible(screenObject)) return;
    const uint32_t completedJob = pendingJob;
    history_query_service::UsageResultView result{};
    history_query_service::Timing timing{};
    if (!history_query_service::acquireUsage(completedJob, result, &timing)) {
        if (history_query_service::jobState(pendingJob) ==
            history_query_service::JobState::Gone) {
            pendingJob = 0;
            startQuery();
        }
        return;
    }
    linear_progress::hide(progress);
    renderChart(result.buckets, result.count, result.status);
    history_query_service::releaseUsage(completedJob);
    pendingJob = 0;
    scheduleNextRefresh();
}

void autoRefreshCb(lv_timer_t*) {
    if (!screenObject || !lv_obj_is_visible(screenObject)) return;
    const bool hasTime = time_service::hasCurrentTime();
    if (hasTime != rangeOptionsHaveTime) {
        updateRangeOptions(hasTime);
        nextRefreshAtMs = 0;
        renderedBucketMinutes = 0;
        startQuery(true);
        return;
    }
    if (pendingJob || !nextRefreshAtMs) return;
    if (static_cast<int32_t>(millis() - nextRefreshAtMs) < 0) return;
    startQuery();
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
    if (selected >= visibleRangeCount) return;
    selectedRange = visibleRanges[selected];
    nextRefreshAtMs = 0;
    renderedBucketMinutes = 0;
    startQuery(true);
}

void screenRefreshCb(lv_event_t* event) {
    // Replace a stale hidden-screen query with one for the currently selected
    // range when this screen becomes active again.
    if (lv_event_get_code(event) != LV_EVENT_REFRESH) return;
    const bool hasTime = time_service::hasCurrentTime();
    if (hasTime != rangeOptionsHaveTime) updateRangeOptions(hasTime);
    startQuery(true);
}

lv_obj_t* addLegendItem(lv_obj_t* parent, lv_color_t color, const char* text)
{
    lv_obj_t* label = lv_label_create(parent);
    lv_obj_set_style_text_color(label, color, 0);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_12, 0);
    lv_label_set_text(label, text);
    return label;
}

} // namespace

lv_obj_t* create(lv_obj_t* parent)
{
    showBalance = sensors::mapping::balanceVisible();
    lv_obj_t* screen = lv_obj_create(parent);
    screenObject = screen;
    // Usage is chart-first: compact controls leave the rest of the page for
    // the data rather than a visually empty footer.
    ui_theme::styleScreen(screen, 4);
    lv_obj_set_style_pad_row(screen, 4, 0);
    lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);
    if (!ensureChartStorage()) {
        lv_obj_t* error = lv_label_create(screen);
        lv_label_set_text(error, "Usage chart memory unavailable");
        lv_obj_center(error);
        return screen;
    }

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
    if (hardware_profile::kControllerIsPwm) {
        for (uint8_t i = 0; i < 5; ++i) chartSeries[i].color = seriesColor(i);
        addLegendItem(legend, seriesColor(0), "Charge");
        addLegendItem(legend, seriesColor(3), "Battery");
        addLegendItem(legend, seriesColor(1), "Solar");
        addLegendItem(legend, seriesColor(4), "Load");
        addLegendItem(legend, seriesColor(2), "Surplus");
    } else {
        rangeSeries[0].color = seriesColor(0);
        rangeSeries[1].color = seriesColor(1);
        rangeSeries[2].color = seriesColor(4);
        rangeSeries[3].color = seriesColor(3);
        rangeSeries[4].color = balanceColor();
        addLegendItem(legend, rangeSeries[0].color, "Charge");
        addLegendItem(legend, rangeSeries[1].color, "Solar In");
        addLegendItem(legend, rangeSeries[2].color, "Solar Use");
        addLegendItem(legend, rangeSeries[3].color, "Bat Use");
        balanceLegendItem =
            addLegendItem(legend, rangeSeries[4].color, "Balance");
        lv_obj_add_flag(balanceLegendItem, LV_OBJ_FLAG_HIDDEN);
    }

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

    updateRangeOptions(time_service::hasCurrentTime());
    // This timer only transfers completed background results into LVGL; it
    // never performs filesystem work or aggregation.
    lv_timer_create(completionCb, 40, nullptr);
    // The cadence timer is also cheap: it only checks visibility and queues a
    // worker job after an x-axis bucket boundary.
    lv_timer_create(autoRefreshCb, kRefreshCheckMs, nullptr);
    return screen;
}

} // namespace usage_screen
