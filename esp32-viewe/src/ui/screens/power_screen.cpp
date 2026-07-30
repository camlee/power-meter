#include "power_screen.h"

#include "../../data/power_flow.h"
#include "../../sensors/sensor_mapping.h"
#include "../../sensors/sensor_mode.h"
#include "../../sensors/sensors.h"
#include "../theme/ui_theme.h"

#include <algorithm>
#include <cmath>

namespace power_screen {
namespace {

constexpr size_t kPoints = 240; // two minutes at the 500 ms sensor cadence
constexpr uint32_t kRefreshMs = 500;
constexpr uint8_t kSolarSeries = 0;
constexpr uint8_t kLoadSeries = 1;
constexpr uint8_t kFlowSeries = 2; // Battery when mapped, otherwise Net
constexpr uint8_t kBalanceSeries = 3;
constexpr uint8_t kMaxSeries = 4;

lv_obj_t* plot = nullptr;
lv_obj_t* kpiValues[kMaxSeries]{};
float values[kMaxSeries][kPoints]{};
sensors::Reading samples[sensors::SENSOR_COUNT][kPoints]{};
size_t count = 0;
float minimum = -1.0f;
float maximum = 1.0f;
float step = 1.0f;
bool batteryMapped = true;
bool showBalance = false;

float nice(float value) {
    const float magnitude =
        powf(10.0f, floorf(log10f(fmaxf(value, 1.0f))));
    const float normalized = value / magnitude;
    const float base = normalized <= 1.0f ? 1.0f :
                       normalized <= 2.0f ? 2.0f :
                       normalized <= 5.0f ? 5.0f : 10.0f;
    return base * magnitude;
}

uint8_t visibleSeriesCount() {
    return static_cast<uint8_t>(
        3 + (batteryMapped && showBalance ? 1 : 0));
}

int yFor(const lv_area_t& area, float value) {
    const int top = area.y1 + 18;
    const int bottom = area.y2 - 24;
    return top + lroundf(
        (maximum - value) * (bottom - top) / (maximum - minimum));
}

void drawLabel(lv_draw_ctx_t* context, const char* text, int x, int y,
               lv_color_t color) {
    lv_draw_label_dsc_t descriptor;
    lv_draw_label_dsc_init(&descriptor);
    descriptor.font = &lv_font_montserrat_14;
    descriptor.color = color;
    lv_area_t area{
        static_cast<lv_coord_t>(x), static_cast<lv_coord_t>(y),
        static_cast<lv_coord_t>(x + 90), static_cast<lv_coord_t>(y + 16)};
    lv_draw_label(context, &descriptor, &area, text, nullptr);
}

void drawCb(lv_event_t* event) {
    if (lv_event_get_code(event) != LV_EVENT_DRAW_MAIN) return;
    auto* context = lv_event_get_draw_ctx(event);
    const auto& area = lv_event_get_target(event)->coords;
    const int left = area.x1 + 42;
    const int right = area.x2 - 4;
    const int top = area.y1 + 18;
    const int bottom = area.y2 - 24;
    const int width = right - left;

    lv_draw_line_dsc_t grid;
    lv_draw_line_dsc_init(&grid);
    grid.color = lv_palette_lighten(LV_PALETTE_GREY, 2);
    grid.width = 1;
    for (float tick = minimum; tick <= maximum + step * 0.1f;
         tick += step) {
        const int y = yFor(area, tick);
        lv_point_t start{
            static_cast<lv_coord_t>(left), static_cast<lv_coord_t>(y)};
        lv_point_t end{
            static_cast<lv_coord_t>(right), static_cast<lv_coord_t>(y)};
        grid.width = fabsf(tick) < 0.01f ? 2 : 1;
        lv_draw_line(context, &grid, &start, &end);
        char text[12];
        lv_snprintf(text, sizeof(text), "%d W",
                    static_cast<int>(lroundf(tick)));
        drawLabel(context, text, area.x1 + 1, y - 7,
                  lv_palette_main(LV_PALETTE_GREY));
    }

    for (int tick = 0; tick <= 2; ++tick) {
        const int x = left + width * tick / 2;
        lv_point_t start{
            static_cast<lv_coord_t>(x), static_cast<lv_coord_t>(top)};
        lv_point_t end{
            static_cast<lv_coord_t>(x), static_cast<lv_coord_t>(bottom)};
        grid.width = 1;
        lv_draw_line(context, &grid, &start, &end);
        drawLabel(context,
                  tick == 2 ? "now" : tick == 1 ? "-1m" : "-2m",
                  tick == 2 ? right - 34 : x - 12, bottom + 5,
                  lv_palette_main(LV_PALETTE_GREY));
    }

    const lv_color_t colors[kMaxSeries] = {
        lv_color_hex(0x0000FF),
        lv_color_hex(0xFFA500),
        lv_color_hex(0x00BFFF),
        lv_color_hex(0x8A949A),
    };
    const size_t firstPoint = count < kPoints ? kPoints - count : 0;
    for (uint8_t series = 0; series < visibleSeriesCount(); ++series) {
        lv_draw_line_dsc_t line;
        lv_draw_line_dsc_init(&line);
        line.color = colors[series];
        line.width = 2;
        for (size_t point = 1; point < count; ++point) {
            if (!std::isfinite(values[series][point - 1]) ||
                !std::isfinite(values[series][point])) {
                continue;
            }
            const int x1 = left + static_cast<int>(
                (firstPoint + point - 1) * width / (kPoints - 1));
            const int x2 = left + static_cast<int>(
                (firstPoint + point) * width / (kPoints - 1));
            lv_point_t start{
                static_cast<lv_coord_t>(x1),
                static_cast<lv_coord_t>(
                    yFor(area, values[series][point - 1]))};
            lv_point_t end{
                static_cast<lv_coord_t>(x2),
                static_cast<lv_coord_t>(
                    yFor(area, values[series][point]))};
            lv_draw_line(context, &line, &start, &end);
        }
    }
}

void createKpi(lv_obj_t* parent, uint8_t index, lv_color_t color,
               const char* name) {
    lv_obj_t* item = lv_obj_create(parent);
    lv_obj_remove_style_all(item);
    lv_obj_set_size(item, 0, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(item, 1);
    lv_obj_set_flex_flow(item, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(item, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* label = lv_label_create(item);
    lv_obj_set_style_text_color(label, color, 0);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
    lv_label_set_text(label, name);

    lv_obj_t* valueRow = lv_obj_create(item);
    lv_obj_remove_style_all(valueRow);
    lv_obj_set_size(valueRow, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_column(valueRow, 2, 0);
    lv_obj_set_flex_flow(valueRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(valueRow, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);

    kpiValues[index] = lv_label_create(valueRow);
    lv_obj_set_style_text_font(
        kpiValues[index], &lv_font_montserrat_28, 0);
    lv_label_set_text(kpiValues[index], "--");

    lv_obj_t* unit = lv_label_create(valueRow);
    lv_obj_set_style_text_color(unit, ui_theme::mutedText(), 0);
    lv_obj_set_style_text_font(unit, &lv_font_montserrat_14, 0);
    lv_obj_set_style_pad_bottom(unit, 3, 0);
    lv_label_set_text(unit, "W");
}

void update(lv_timer_t*) {
    const size_t solarCount =
        sensors::getRecent(sensors::SENSOR_SOLAR, samples[0], kPoints);
    const size_t loadCount =
        sensors::getRecent(sensors::SENSOR_LOAD, samples[1], kPoints);
    const size_t batteryCount = batteryMapped
        ? sensors::getRecent(sensors::SENSOR_BATTERY, samples[2], kPoints)
        : 0;
    size_t sampleCount = std::min(solarCount, loadCount);
    if (batteryMapped) sampleCount = std::min(sampleCount, batteryCount);
    count = sampleCount;

    float low = 0.0f;
    float high = 0.0f;
    for (size_t point = 0; point < sampleCount; ++point) {
        values[kSolarSeries][point] =
            sensors::isCalculationEligible(samples[0][point])
                ? samples[0][point].power : NAN;
        values[kLoadSeries][point] =
            sensors::isCalculationEligible(samples[1][point])
                ? samples[1][point].power : NAN;
        if (batteryMapped) {
            values[kFlowSeries][point] =
                sensors::isCalculationEligible(samples[2][point])
                    ? samples[2][point].power : NAN;
            values[kBalanceSeries][point] = power_flow::balance(
                values[kSolarSeries][point], values[kLoadSeries][point],
                values[kFlowSeries][point]);
        } else {
            values[kFlowSeries][point] =
                std::isfinite(values[kSolarSeries][point]) &&
                std::isfinite(values[kLoadSeries][point])
                    ? values[kSolarSeries][point] -
                          values[kLoadSeries][point]
                    : NAN;
            values[kBalanceSeries][point] = NAN;
        }
        for (uint8_t series = 0; series < visibleSeriesCount(); ++series) {
            if (!std::isfinite(values[series][point])) continue;
            low = std::min(low, values[series][point]);
            high = std::max(high, values[series][point]);
        }
    }

    step = nice((high - low) / 6.0f);
    maximum = ceilf(high / step) * step;
    minimum = floorf(low / step) * step;
    if (maximum <= minimum) {
        maximum = step;
        minimum = -step;
    }

    for (uint8_t series = 0; series < visibleSeriesCount(); ++series) {
        char text[12];
        if (sampleCount &&
            std::isfinite(values[series][sampleCount - 1])) {
            lv_snprintf(
                text, sizeof(text), "%d",
                static_cast<int>(lroundf(values[series][sampleCount - 1])));
        } else {
            lv_snprintf(text, sizeof(text), "--");
        }
        lv_label_set_text(kpiValues[series], text);
    }
    lv_obj_invalidate(plot);
}

void visibleUpdate(lv_timer_t* timer) {
    if (!timer || !timer->user_data ||
        !lv_obj_is_visible(static_cast<lv_obj_t*>(timer->user_data))) {
        return;
    }
    update(timer);
}

} // namespace

lv_obj_t* create(lv_obj_t* parent) {
    sensors::mapping::PhysicalSensorId physical{};
    batteryMapped = sensors::mapping::physicalForLogical(
        sensor_mode::get(), sensors::SENSOR_BATTERY, physical);
    showBalance = batteryMapped && sensors::mapping::balanceVisible();

    lv_obj_t* screen = lv_obj_create(parent);
    ui_theme::styleScreen(screen, 6);
    lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(screen, 4, 0);

    lv_obj_t* row = lv_obj_create(screen);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    createKpi(row, kSolarSeries, lv_color_hex(0x0000FF), "Solar");
    createKpi(row, kLoadSeries, lv_color_hex(0xFFA500), "Load");
    createKpi(row, kFlowSeries, lv_color_hex(0x00BFFF),
              batteryMapped ? "Bat" : "Net");
    if (showBalance) {
        createKpi(row, kBalanceSeries, lv_color_hex(0x8A949A), "Balance");
    }

    plot = lv_obj_create(screen);
    lv_obj_remove_style_all(plot);
    lv_obj_set_width(plot, lv_pct(100));
    lv_obj_set_flex_grow(plot, 1);
    lv_obj_add_event_cb(plot, drawCb, LV_EVENT_DRAW_MAIN, nullptr);
    lv_timer_create(visibleUpdate, kRefreshMs, screen);
    update(nullptr);
    return screen;
}

} // namespace power_screen
