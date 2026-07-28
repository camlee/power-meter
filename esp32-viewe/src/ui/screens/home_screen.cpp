#include "home_screen.h"

#include "../../sensors/sensors.h"
#include "../display_brightness.h"
#include "../theme/ui_theme.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace home_screen {
namespace {

constexpr uint32_t kRefreshMs = sensors::kSampleIntervalMs;
constexpr size_t kPointCount = 60;
constexpr float kSoftMinimum = -60.0f;
constexpr float kSoftMaximum = 60.0f;
constexpr float kThresholds[] = {-50.0f, -25.0f, 0.0f, 25.0f, 50.0f};

lv_obj_t* screenObject = nullptr;
lv_obj_t* stateLabel = nullptr;
lv_obj_t* powerValue = nullptr;
lv_obj_t* plot = nullptr;
lv_obj_t* batteryVoltage = nullptr;
lv_obj_t* brightnessSlider = nullptr;
lv_obj_t* brightnessValueLabel = nullptr;
float values[kPointCount]{};
size_t valueCount = 0;
float axisMinimum = kSoftMinimum;
float axisMaximum = kSoftMaximum;
sensors::Reading readings[sensors::SENSOR_COUNT][kPointCount]{};

lv_color_t bandColor(float watts) {
    if (watts < -50.0f) return lv_color_hex(ui_theme::isDark() ? 0xF07A70 : 0xC33B32);
    if (watts < -5.0f) return lv_color_hex(ui_theme::isDark() ? 0xE0A447 : 0xB86F00);
    if (watts < 5.0f) return ui_theme::mutedText();
    if (watts <= 50.0f) return lv_color_hex(ui_theme::isDark() ? 0x55C982 : 0x168447);
    return lv_color_hex(ui_theme::isDark() ? 0x5596E6 : 0x1464DF);
}

int yFor(const lv_area_t& area, float value) {
    const int top = area.y1 + 16;
    const int bottom = area.y2 - 24;
    return top + lroundf((axisMaximum - value) * (bottom - top) /
                         (axisMaximum - axisMinimum));
}

void drawLabel(lv_draw_ctx_t* ctx, const char* text, int x, int y,
               lv_color_t color, lv_text_align_t align = LV_TEXT_ALIGN_LEFT) {
    lv_draw_label_dsc_t dsc;
    lv_draw_label_dsc_init(&dsc);
    dsc.font = &lv_font_montserrat_12;
    dsc.color = color;
    dsc.align = align;
    lv_area_t area{static_cast<lv_coord_t>(x), static_cast<lv_coord_t>(y),
                   static_cast<lv_coord_t>(x + 90), static_cast<lv_coord_t>(y + 15)};
    lv_draw_label(ctx, &dsc, &area, text, nullptr);
}

void drawPlot(lv_event_t* event) {
    if (lv_event_get_code(event) != LV_EVENT_DRAW_MAIN) return;
    lv_draw_ctx_t* ctx = lv_event_get_draw_ctx(event);
    const lv_area_t& area = lv_event_get_target(event)->coords;
    const int left = area.x1 + 43;
    const int right = area.x2 - 5;
    const int top = area.y1 + 16;
    const int bottom = area.y2 - 24;
    const int width = right - left;
    if (width < 2 || bottom <= top) return;

    lv_draw_line_dsc_t grid;
    lv_draw_line_dsc_init(&grid);
    grid.color = ui_theme::border();
    grid.width = 1;
    for (float threshold : kThresholds) {
        if (threshold < axisMinimum || threshold > axisMaximum) continue;
        const int y = yFor(area, threshold);
        lv_point_t p1{static_cast<lv_coord_t>(left), static_cast<lv_coord_t>(y)};
        lv_point_t p2{static_cast<lv_coord_t>(right), static_cast<lv_coord_t>(y)};
        grid.width = threshold == 0.0f ? 2 : 1;
        lv_draw_line(ctx, &grid, &p1, &p2);
        if (threshold == 0.0f) continue;
        char text[12];
        lv_snprintf(text, sizeof(text), "%d W", static_cast<int>(threshold));
        drawLabel(ctx, text, area.x1, y - 7, ui_theme::mutedText());
    }

    for (int tick = 0; tick < 3; ++tick) {
        const int x = left + width * tick / 2;
        lv_point_t p1{static_cast<lv_coord_t>(x), static_cast<lv_coord_t>(top)};
        lv_point_t p2{static_cast<lv_coord_t>(x), static_cast<lv_coord_t>(bottom)};
        grid.width = 1;
        lv_draw_line(ctx, &grid, &p1, &p2);
        drawLabel(ctx, tick == 0 ? "-30s" : tick == 1 ? "-15s" : "now",
                  tick == 2 ? right - 30 : x - 14, bottom + 5, ui_theme::mutedText());
    }

    // Keep the sign clear of the right-edge grid line; otherwise the
    // discharging minus visually turns into a plus.
    drawLabel(ctx, "Charging +", right - 96, top, ui_theme::mutedText(), LV_TEXT_ALIGN_RIGHT);
    drawLabel(ctx, "Discharging -", right - 96, bottom - 15,
              ui_theme::mutedText(), LV_TEXT_ALIGN_RIGHT);

    if (valueCount < 2) return;
    const size_t firstPoint = valueCount < kPointCount ? kPointCount - valueCount : 0;
    for (size_t i = 1; i < valueCount; ++i) {
        if (!std::isfinite(values[i - 1]) || !std::isfinite(values[i])) continue;
        const int x1 = left + static_cast<int>((firstPoint + i - 1) * width / (kPointCount - 1));
        const int x2 = left + static_cast<int>((firstPoint + i) * width / (kPointCount - 1));
        lv_draw_line_dsc_t line;
        lv_draw_line_dsc_init(&line);
        line.color = bandColor((values[i - 1] + values[i]) * 0.5f);
        line.width = 3;
        lv_point_t p1{static_cast<lv_coord_t>(x1),
                      static_cast<lv_coord_t>(yFor(area, values[i - 1]))};
        lv_point_t p2{static_cast<lv_coord_t>(x2),
                      static_cast<lv_coord_t>(yFor(area, values[i]))};
        lv_draw_line(ctx, &line, &p1, &p2);
    }
}

bool eligibleVoltage(sensors::SensorId id, float& voltage) {
    sensors::Reading reading{};
    if (!sensors::getLatest(id, reading) || !sensors::isCalculationEligible(reading)) return false;
    voltage = reading.voltage;
    return std::isfinite(voltage);
}

void updateBrightnessLabel() {
    if (!brightnessValueLabel) return;
    char text[8];
    lv_snprintf(text, sizeof(text), "%d%%", display_brightness::get());
    lv_label_set_text(brightnessValueLabel, text);
}

void syncBrightnessControl() {
    if (!brightnessSlider) return;
    if (!lv_obj_has_state(brightnessSlider, LV_STATE_PRESSED)) {
        lv_slider_set_value(brightnessSlider, display_brightness::get(), LV_ANIM_OFF);
    }
    updateBrightnessLabel();
}

void brightnessChanged(lv_event_t* event) {
    const lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_VALUE_CHANGED) {
        display_brightness::set(lv_slider_get_value(brightnessSlider));
        updateBrightnessLabel();
    } else if (code == LV_EVENT_RELEASED) {
        display_brightness::save();
    }
}

void update(lv_timer_t*) {
    if (display_brightness::update()) syncBrightnessControl();
    if (!screenObject || !lv_obj_is_visible(screenObject)) return;

    const size_t solarCount =
        sensors::getRecent(sensors::SENSOR_IN, readings[sensors::SENSOR_IN], kPointCount);
    const size_t loadCount =
        sensors::getRecent(sensors::SENSOR_OUT, readings[sensors::SENSOR_OUT], kPointCount);
    const size_t batteryCount =
        sensors::getRecent(sensors::SENSOR_AUX, readings[sensors::SENSOR_AUX], kPointCount);
    valueCount = std::max(batteryCount, std::min(solarCount, loadCount));
    if (valueCount > kPointCount) valueCount = kPointCount;

    float low = kSoftMinimum;
    float high = kSoftMaximum;
    for (size_t i = 0; i < valueCount; ++i) {
        const size_t batteryOffset = valueCount > batteryCount ? valueCount - batteryCount : 0;
        const size_t flowOffset = valueCount > std::min(solarCount, loadCount)
            ? valueCount - std::min(solarCount, loadCount) : 0;
        float value = NAN;
        if (i >= batteryOffset) {
            const auto& battery = readings[sensors::SENSOR_AUX][i - batteryOffset];
            if (sensors::isCalculationEligible(battery)) value = battery.power;
        }
        if (!std::isfinite(value) && i >= flowOffset) {
            const auto& solar = readings[sensors::SENSOR_IN][i - flowOffset];
            const auto& load = readings[sensors::SENSOR_OUT][i - flowOffset];
            if (sensors::isCalculationEligible(solar) && sensors::isCalculationEligible(load)) {
                value = solar.power - load.power;
            }
        }
        values[i] = value;
        if (std::isfinite(value)) {
            low = std::min(low, value);
            high = std::max(high, value);
        }
    }
    axisMinimum = std::min(kSoftMinimum, floorf(low / 20.0f) * 20.0f);
    axisMaximum = std::max(kSoftMaximum, ceilf(high / 20.0f) * 20.0f);

    float net = NAN;
    if (sensors::getSystemNetPower(net)) {
        char text[20];
        snprintf(text, sizeof(text), "%+.0f", static_cast<double>(net));
        lv_label_set_text(powerValue, text);
        lv_obj_set_style_text_color(powerValue, bandColor(net), 0);
        lv_label_set_text(stateLabel, net > 0.0f ? "Charging" :
                                      net < 0.0f ? "Discharging" : "Balanced");
        lv_obj_set_style_text_color(stateLabel, bandColor(net), 0);
    } else {
        lv_label_set_text(powerValue, "--");
        lv_obj_set_style_text_color(powerValue, ui_theme::mutedText(), 0);
        lv_label_set_text(stateLabel, "Error - missing sensors");
        lv_obj_set_style_text_color(stateLabel, lv_color_hex(
            ui_theme::isDark() ? 0xF07A70 : 0xC33B32), 0);
    }

    float voltage = NAN;
    const bool directBattery = eligibleVoltage(sensors::SENSOR_AUX, voltage);
    if (!directBattery) eligibleVoltage(sensors::SENSOR_OUT, voltage);
    char text[24];
    if (std::isfinite(voltage)) {
        snprintf(text, sizeof(text), "%.1f V%s",
                 static_cast<double>(voltage), directBattery ? "" : " (Load)");
    } else {
        snprintf(text, sizeof(text), "-- V");
    }
    lv_label_set_text(batteryVoltage, text);
    lv_obj_invalidate(plot);
}

} // namespace

lv_obj_t* create(lv_obj_t* parent) {
    lv_obj_t* screen = lv_obj_create(parent);
    screenObject = screen;
    ui_theme::styleScreen(screen, 6);
    lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(screen, 2, 0);

    lv_obj_t* summaryRow = lv_obj_create(screen);
    lv_obj_remove_style_all(summaryRow);
    lv_obj_set_size(summaryRow, lv_pct(100), 57);
    lv_obj_set_flex_flow(summaryRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(summaryRow, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(summaryRow, 6, 0);

    stateLabel = lv_label_create(summaryRow);
    lv_obj_set_flex_grow(stateLabel, 1);
    lv_label_set_long_mode(stateLabel, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(stateLabel, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_font(stateLabel, &lv_font_montserrat_28, 0);
    lv_label_set_text(stateLabel, "Error - missing sensors");

    lv_obj_t* powerRow = lv_obj_create(summaryRow);
    lv_obj_remove_style_all(powerRow);
    lv_obj_set_size(powerRow, LV_SIZE_CONTENT, 57);
    lv_obj_set_flex_flow(powerRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(powerRow, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_column(powerRow, 4, 0);
    powerValue = lv_label_create(powerRow);
    lv_obj_set_style_text_font(powerValue, &lv_font_montserrat_48, 0);
    lv_label_set_text(powerValue, "--");
    lv_obj_t* unit = lv_label_create(powerRow);
    lv_obj_set_style_text_font(unit, &lv_font_montserrat_20, 0);
    lv_obj_set_style_pad_bottom(unit, 6, 0);
    lv_obj_set_style_text_color(unit, ui_theme::mutedText(), 0);
    lv_label_set_text(unit, "W");

    lv_obj_t* batteryRow = lv_obj_create(screen);
    lv_obj_remove_style_all(batteryRow);
    lv_obj_set_size(batteryRow, lv_pct(100), 24);
    lv_obj_set_flex_flow(batteryRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(batteryRow, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(batteryRow, 8, 0);

    lv_obj_t* batteryLabel = lv_label_create(batteryRow);
    lv_obj_set_style_text_font(batteryLabel, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(batteryLabel, ui_theme::mutedText(), 0);
    lv_label_set_text(batteryLabel, "Battery");

    batteryVoltage = lv_label_create(batteryRow);
    lv_obj_set_style_text_font(batteryVoltage, &lv_font_montserrat_20, 0);
    lv_label_set_text(batteryVoltage, "-- V");

    plot = lv_obj_create(screen);
    lv_obj_remove_style_all(plot);
    lv_obj_set_width(plot, lv_pct(100));
    lv_obj_set_flex_grow(plot, 1);
    lv_obj_add_event_cb(plot, drawPlot, LV_EVENT_DRAW_MAIN, nullptr);

    lv_obj_t* brightnessRow = lv_obj_create(screen);
    lv_obj_remove_style_all(brightnessRow);
    lv_obj_set_size(brightnessRow, lv_pct(100), 30);
    lv_obj_set_flex_flow(brightnessRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(brightnessRow, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_left(brightnessRow, 4, 0);
    lv_obj_set_style_pad_right(brightnessRow, 2, 0);
    lv_obj_set_style_pad_column(brightnessRow, 10, 0);

    lv_obj_t* brightnessLabel = lv_label_create(brightnessRow);
    lv_label_set_text(brightnessLabel, "Brightness");
    lv_obj_set_style_text_color(brightnessLabel, ui_theme::mutedText(), 0);

    brightnessSlider = lv_slider_create(brightnessRow);
    lv_obj_set_height(brightnessSlider, 10);
    lv_obj_set_flex_grow(brightnessSlider, 1);
    lv_obj_set_ext_click_area(brightnessSlider, 10);
    lv_slider_set_range(brightnessSlider, display_brightness::kMinimumPercent,
                        display_brightness::kMaximumPercent);
    lv_slider_set_value(brightnessSlider, display_brightness::get(), LV_ANIM_OFF);
    lv_obj_set_style_bg_color(brightnessSlider, ui_theme::surfaceAlt(), LV_PART_MAIN);
    lv_obj_set_style_bg_color(brightnessSlider, ui_theme::accent(), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(brightnessSlider, ui_theme::accent(), LV_PART_KNOB);
    lv_obj_add_event_cb(brightnessSlider, brightnessChanged, LV_EVENT_ALL, nullptr);

    brightnessValueLabel = lv_label_create(brightnessRow);
    lv_obj_set_width(brightnessValueLabel, 38);
    lv_obj_set_style_text_align(brightnessValueLabel, LV_TEXT_ALIGN_RIGHT, 0);
    updateBrightnessLabel();

    lv_timer_create(update, kRefreshMs, nullptr);
    update(nullptr);
    return screen;
}

} // namespace home_screen
