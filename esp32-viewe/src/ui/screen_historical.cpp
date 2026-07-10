#include "screen_historical.h"
#include "../data/historical_storage.h"
#include <cstdio>

namespace screen_historical {
namespace {

// New data only lands once a minute, so a slow poll here is fine -- no
// dedup/shift-mode complexity needed like the realtime screen.
constexpr uint32_t kRefreshIntervalMs = 30000;
constexpr size_t kDisplayMinutes = 180; // 3-hour visible window
constexpr lv_palette_t kSensorColor[historical_storage::kSensorCount] = {
    LV_PALETTE_RED, LV_PALETTE_GREEN, LV_PALETTE_BLUE
};

lv_obj_t* chart = nullptr;
lv_chart_series_t* series[historical_storage::kSensorCount] = {nullptr, nullptr, nullptr};
lv_obj_t* energyLabels[historical_storage::kSensorCount] = {nullptr, nullptr, nullptr};
lv_timer_t* refreshTimer = nullptr;

historical_storage::MinuteRecord records[kDisplayMinutes];

void refreshCb(lv_timer_t*) {
    size_t n = historical_storage::getRecent(records, kDisplayMinutes);

    // Full redraw each refresh -- simplest correct approach at a 30s-or-slower
    // cadence. Pad the front with "no data yet" so the chart doesn't look
    // like history starts wherever the window happens to begin.
    size_t pad = kDisplayMinutes - n;
    for (size_t i = 0; i < pad; i++) {
        for (uint8_t s = 0; s < historical_storage::kSensorCount; s++) {
            lv_chart_set_value_by_id(chart, series[s], i, LV_CHART_POINT_NONE);
        }
    }
    for (size_t i = 0; i < n; i++) {
        for (uint8_t s = 0; s < historical_storage::kSensorCount; s++) {
            lv_chart_set_value_by_id(chart, series[s], pad + i, (lv_coord_t)records[i].avgPowerW[s]);
        }
    }
    lv_chart_refresh(chart);

    // Lifetime energy total per sensor across everything currently stored
    // (i.e. up to kMaxRecords worth of history, not just the visible window).
    // For a "since midnight" or "last 24h" figure instead, filter by
    // epoch_s once a real RTC/NTP time source is wired into historical_storage.
    double totalWh[historical_storage::kSensorCount] = {0, 0, 0};
    for (size_t i = 0; i < n; i++) {
        for (uint8_t s = 0; s < historical_storage::kSensorCount; s++) {
            totalWh[s] += records[i].energyWh[s];
        }
    }
    for (uint8_t s = 0; s < historical_storage::kSensorCount; s++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "S%u: %.2f Wh", (unsigned)(s + 1), totalWh[s]);
        lv_label_set_text(energyLabels[s], buf);
    }
}

} // namespace

lv_obj_t* create(lv_obj_t* parent) {
    lv_obj_t* scr = lv_obj_create(parent);
    lv_obj_set_size(scr, lv_pct(100), lv_pct(100));
    lv_obj_set_style_pad_all(scr, 4, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);

    lv_obj_t* title = lv_label_create(scr);
    lv_label_set_text(title, "Power History (last 3h)");

    lv_obj_t* energyRow = lv_obj_create(scr);
    lv_obj_set_size(energyRow, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_border_width(energyRow, 0, 0);
    lv_obj_set_flex_flow(energyRow, LV_FLEX_FLOW_ROW);
    for (uint8_t s = 0; s < historical_storage::kSensorCount; s++) {
        energyLabels[s] = lv_label_create(energyRow);
        lv_label_set_text(energyLabels[s], "S? -- Wh");
    }

    chart = lv_chart_create(scr);
    lv_obj_set_size(chart, lv_pct(100), lv_pct(75));
    lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
    lv_chart_set_update_mode(chart, LV_CHART_UPDATE_MODE_CIRCULAR);
    lv_chart_set_point_count(chart, kDisplayMinutes);
    lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, 0, 100); // watts; tune once real ranges known

    for (uint8_t s = 0; s < historical_storage::kSensorCount; s++) {
        series[s] = lv_chart_add_series(chart, lv_palette_main(kSensorColor[s]), LV_CHART_AXIS_PRIMARY_Y);
    }

    refreshTimer = lv_timer_create(refreshCb, kRefreshIntervalMs, nullptr);
    refreshCb(nullptr); // populate immediately instead of waiting for the first tick

    return scr;
}

} // namespace screen_historical
