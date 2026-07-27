#pragma once

#include <lvgl.h>
#include <cstddef>
#include <cstdint>

namespace stacked_bar_chart {

constexpr uint8_t kMaxSeries = 6;

// Values are magnitudes. Positive series stack above zero; negative series
// stack below zero, in their declaration order.
struct Series {
    lv_color_t color;
    const float* values;
    bool positive;
};

enum class AxisMode : uint8_t {
    Relative,
    WallClock,
};

struct Data {
    const Series* series;
    uint8_t seriesCount;
    size_t pointCount;
    uint32_t durationMinutes;
    uint32_t tickMinutes;
    const char* yAxisTitle;
    AxisMode axisMode = AxisMode::Relative;
    int64_t axisStartTimeMs = 0;
    int16_t utcOffsetMinutes = 0;
};

lv_obj_t* create(lv_obj_t* parent);
void setData(lv_obj_t* chart, const Data& data);

} // namespace stacked_bar_chart
