#include <cstdio>

#include "screen_realtime.h"
#include "nav_bar.h"
#include "screen_manager.h"
#include "../sensors/sensor_task.h"

namespace screen_realtime {
namespace {

lv_obj_t* chart = nullptr;
lv_chart_series_t* series = nullptr;
lv_obj_t* valueLabel = nullptr;
lv_timer_t* updateTimer = nullptr;

constexpr uint32_t kUpdateIntervalMs = 500;
constexpr size_t kChartPoints = 60; // visible window; must be <= sensor_task::kHistorySize

void updateCb(lv_timer_t*) {
    sensor_task::Reading readings[kChartPoints];
    size_t n = sensor_task::getRecent(readings, kChartPoints);
    if (n == 0) return;

    for (size_t i = 0; i < n; i++) {
        lv_chart_set_next_value(chart, series, (lv_coord_t)readings[i].value);
    }

    char buf[32];
    snprintf(buf, sizeof(buf), "%.1f", readings[n - 1].value);
    lv_label_set_text(valueLabel, buf);
}

} // namespace

lv_obj_t* create() {
    lv_obj_t* scr = lv_obj_create(nullptr);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);

    nav_bar::create(scr, ScreenId::Realtime);

    valueLabel = lv_label_create(scr);
    lv_obj_set_style_text_font(valueLabel, &lv_font_montserrat_32, 0); // enable LV_FONT_MONTSERRAT_32 in lv_conf.h
    lv_label_set_text(valueLabel, "--");

    chart = lv_chart_create(scr);
    lv_obj_set_size(chart, lv_pct(90), lv_pct(60));
    lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
    lv_chart_set_update_mode(chart, LV_CHART_UPDATE_MODE_SHIFT);
    lv_chart_set_point_count(chart, kChartPoints);
    lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, 0, 100); // matches sensor_task's fake range for now
    series = lv_chart_add_series(chart, lv_palette_main(LV_PALETTE_BLUE), LV_CHART_AXIS_PRIMARY_Y);

    updateTimer = lv_timer_create(updateCb, kUpdateIntervalMs, nullptr);

    return scr;
}

} // namespace screen_realtime
