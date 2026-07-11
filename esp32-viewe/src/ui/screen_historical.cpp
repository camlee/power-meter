#include "screen_historical.h"

#include "../data/historical_storage.h"

namespace screen_historical {
namespace {

constexpr size_t kMaxPoints = 180;
constexpr uint32_t kLookbackMinutes = 180;
constexpr uint32_t kRefreshIntervalMs = 30000;

lv_obj_t* chart = nullptr;
lv_chart_series_t* inSeries = nullptr;
lv_chart_series_t* outSeries = nullptr;
lv_chart_series_t* auxSeries = nullptr;
lv_obj_t* statusLabel = nullptr;

void updateChart()
{
    static historical_storage::MinuteRecord records[kMaxPoints];
    const size_t count = historical_storage::getTimeSeries(
        records, kMaxPoints, kLookbackMinutes);

    if (count == 0) {
        lv_chart_set_point_count(chart, 1);
        lv_chart_set_value_by_id(chart, inSeries, 0, LV_CHART_POINT_NONE);
        lv_chart_set_value_by_id(chart, outSeries, 0, LV_CHART_POINT_NONE);
        lv_chart_set_value_by_id(chart, auxSeries, 0, LV_CHART_POINT_NONE);
        lv_label_set_text(statusLabel, "Waiting for the first completed minute.");
        lv_chart_refresh(chart);
        return;
    }

    lv_chart_set_point_count(chart, count);
    for (size_t i = 0; i < count; ++i) {
        lv_chart_set_value_by_id(chart, inSeries, i,
            static_cast<lv_coord_t>(records[i].avgPowerW[0]));
        lv_chart_set_value_by_id(chart, outSeries, i,
            static_cast<lv_coord_t>(records[i].avgPowerW[1]));
        lv_chart_set_value_by_id(chart, auxSeries, i,
            static_cast<lv_coord_t>(records[i].avgPowerW[2]));
    }

    char status[72];
    lv_snprintf(status, sizeof(status), "%u minute%s shown; newest values may still be in RAM.",
        static_cast<unsigned>(count), count == 1 ? "" : "s");
    lv_label_set_text(statusLabel, status);
    lv_chart_refresh(chart);
}

void refreshCb(lv_timer_t*)
{
    updateChart();
}

void addLegendItem(lv_obj_t* parent, lv_palette_t palette, const char* text)
{
    lv_obj_t* item = lv_obj_create(parent);
    lv_obj_remove_style_all(item);
    lv_obj_set_size(item, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(item, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(item, 4, 0);

    lv_obj_t* dot = lv_obj_create(item);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, 10, 10);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, lv_palette_main(palette), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);

    lv_obj_t* label = lv_label_create(item);
    lv_label_set_text(label, text);
}

} // namespace

lv_obj_t* create(lv_obj_t* parent)
{
    lv_obj_t* screen = lv_obj_create(parent);
    lv_obj_remove_style_all(screen);
    lv_obj_set_size(screen, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(screen, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(screen, 8, 0);
    lv_obj_set_style_pad_row(screen, 6, 0);
    lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);

    lv_obj_t* title = lv_label_create(screen);
    lv_label_set_text(title, "History — average power");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);

    lv_obj_t* legend = lv_obj_create(screen);
    lv_obj_remove_style_all(legend);
    lv_obj_set_size(legend, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(legend, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(legend, 12, 0);
    addLegendItem(legend, LV_PALETTE_BLUE, "In");
    addLegendItem(legend, LV_PALETTE_ORANGE, "Out");
    addLegendItem(legend, LV_PALETTE_GREEN, "Aux");

    chart = lv_chart_create(screen);
    lv_obj_set_width(chart, lv_pct(100));
    lv_obj_set_flex_grow(chart, 1);
    lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
    lv_chart_set_update_mode(chart, LV_CHART_UPDATE_MODE_SHIFT);
    lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
    lv_chart_set_div_line_count(chart, 5, 4);
    lv_obj_set_style_line_width(chart, 2, LV_PART_ITEMS);
    lv_obj_set_style_size(chart, 0, LV_PART_INDICATOR);

    inSeries = lv_chart_add_series(chart, lv_palette_main(LV_PALETTE_BLUE), LV_CHART_AXIS_PRIMARY_Y);
    outSeries = lv_chart_add_series(chart, lv_palette_main(LV_PALETTE_ORANGE), LV_CHART_AXIS_PRIMARY_Y);
    auxSeries = lv_chart_add_series(chart, lv_palette_main(LV_PALETTE_GREEN), LV_CHART_AXIS_PRIMARY_Y);

    statusLabel = lv_label_create(screen);
    lv_obj_set_style_text_color(statusLabel, lv_palette_main(LV_PALETTE_GREY), 0);

    lv_timer_create(refreshCb, kRefreshIntervalMs, nullptr);
    updateChart();
    return screen;
}

} // namespace screen_historical
