#include "usage_screen.h"

#include "../../data/historical_storage.h"
#include "../components/stacked_bar_chart.h"
#include "../theme/ui_theme.h"

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
    uint32_t endOffsetMinutes;
};

constexpr Range kRanges[] = {
    {"Last hour", 60, 2, 0},
    {"Last 6 hours", 360, 15, 0},
    {"Last 12 hours", 720, 15, 0},
    {"Last 24 hours", 1440, 30, 0},
    {"Previous 24 hours", 1440, 30, 1440},
    {"Last 2 days", 2880, 60, 0},
    {"Last week", 10080, 240, 0},
    {"Last 14 days", 20160, 360, 0},
};

lv_obj_t* chart = nullptr;
float chartValues[5][kMaxPoints] = {};
stacked_bar_chart::Series chartSeries[5] = {
    {lv_color_hex(0x159947), chartValues[0], true}, {lv_color_hex(0x0000FF), chartValues[1], true},
    {lv_color_hex(0x00BFFF), chartValues[2], true}, {lv_color_hex(0xFF4500), chartValues[3], false},
    {lv_color_hex(0xFFA500), chartValues[4], false},
};
lv_obj_t* rangeDropdown = nullptr;
lv_obj_t* emptyLabel = nullptr;
uint8_t selectedRange = 0;
uint8_t visibleRanges[sizeof(kRanges) / sizeof(kRanges[0])];
uint8_t visibleRangeCount = 0;

lv_color_t seriesColor(uint8_t index)
{
    // Preserve the familiar energy semantics in both themes, while lifting
    // and softening the traces enough to remain comfortable on a dark panel.
    static constexpr uint32_t light[] = {0x159947, 0x0000FF, 0x00BFFF, 0xFF4500, 0xFFA500};
    static constexpr uint32_t dark[] = {0x3CA76C, 0x5596E6, 0x4DB6D0, 0xE56C63, 0xD99A58};
    return lv_color_hex(ui_theme::isDark() ? dark[index] : light[index]);
}

void formatRelativeAge(char* out, size_t outSize, uint32_t minutes)
{
    if (minutes == 0) {
        lv_snprintf(out, outSize, "now");
    } else if (minutes < 60) {
        lv_snprintf(out, outSize, "-%um", static_cast<unsigned>(minutes));
    } else if (minutes < 1440) {
        lv_snprintf(out, outSize, "-%uh", static_cast<unsigned>(minutes / 60));
    } else {
        lv_snprintf(out, outSize, "-%ud", static_cast<unsigned>(minutes / 1440));
    }
}

void updateRangeOptions()
{
    // Keep the short ranges available from first boot.  Longer choices appear
    // only after there is enough retained history, plus the next useful range.
    const uint32_t availableMinutes = historical_storage::recordCount();
    const uint8_t regularRanges[] = {0, 1, 2, 3, 5, 6, 7};
    visibleRangeCount = 0;

    for (uint8_t pos = 0; pos < sizeof(regularRanges); ++pos) {
        const uint8_t index = regularRanges[pos];
        if (pos < 2 || availableMinutes >= kRanges[index].lookbackMinutes) {
            visibleRanges[visibleRangeCount++] = index;
            continue;
        }
        visibleRanges[visibleRangeCount++] = index; // one range beyond the available data
        break;
    }

    // The previous-day window needs a full additional day of earlier data.
    if (availableMinutes >= 2880) {
        for (uint8_t i = visibleRangeCount; i > 4; --i) visibleRanges[i] = visibleRanges[i - 1];
        visibleRanges[4] = 4;
        ++visibleRangeCount;
    }

    char options[112] = {};
    size_t used = 0;
    uint8_t selectedOption = 0;
    for (uint8_t i = 0; i < visibleRangeCount; ++i) {
        if (i > 0 && used + 1 < sizeof(options)) options[used++] = '\n';
        const int written = lv_snprintf(options + used, sizeof(options) - used, "%s", kRanges[visibleRanges[i]].title);
        if (written > 0) used += static_cast<size_t>(written);
        if (visibleRanges[i] == selectedRange) selectedOption = i;
    }
    lv_dropdown_set_options(rangeDropdown, options);
    lv_dropdown_set_selected(rangeDropdown, selectedOption);
}

void updateChart()
{
    const Range& range = kRanges[selectedRange];
    static historical_storage::PowerBucket buckets[kMaxPoints];
    const size_t count = historical_storage::getPowerBuckets(
        buckets, kMaxPoints, range.lookbackMinutes, range.bucketMinutes, range.endOffsetMinutes);

    updateRangeOptions();
    const size_t expectedPoints = range.lookbackMinutes / range.bucketMinutes;
    for (size_t i = 0; i < expectedPoints; ++i) for (auto& values : chartValues) values[i] = 0;

    // Right-align the completed buckets. The empty columns preserve the full
    // selected duration instead of visually shrinking the time axis at boot.
    const size_t firstPoint = count < expectedPoints ? expectedPoints - count : 0;
    for (size_t i = 0; i < count; ++i) {
        const size_t point = firstPoint + i;
        if (point >= expectedPoints) break;
        const float charge = buckets[i].componentAveragePowerW[historical_storage::BATTERY_CHARGING];
        const float use = buckets[i].componentAveragePowerW[historical_storage::BATTERY_USAGE];
        const float panel = buckets[i].componentAveragePowerW[historical_storage::PANEL_IN];
        const float surplus = buckets[i].componentAveragePowerW[historical_storage::PANEL_SURPLUS];

        chartValues[0][point] = charge;
        chartValues[1][point] = panel;
        chartValues[2][point] = surplus;
        chartValues[3][point] = use;
        chartValues[4][point] = panel;
    }
    if (count == 0) lv_label_set_text(emptyLabel, "No complete intervals yet");
    else lv_label_set_text(emptyLabel, "");
    const uint32_t tickMinutes = range.lookbackMinutes <= 60 ? 15 : range.lookbackMinutes <= 720 ? 60 :
        range.lookbackMinutes <= 1440 ? 180 : range.lookbackMinutes <= 2880 ? 360 : 1440;
    stacked_bar_chart::setData(chart, {chartSeries, 5, expectedPoints, range.lookbackMinutes, tickMinutes, nullptr});
}

void refreshCb(lv_timer_t*) { updateChart(); }

void rangeChangedCb(lv_event_t* event)
{
    const uint16_t selected = lv_dropdown_get_selected(lv_event_get_target(event));
    if (selected >= visibleRangeCount) return;
    selectedRange = visibleRanges[selected];
    updateChart();
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
    // Usage is chart-first: compact controls leave the rest of the page for
    // the data rather than a visually empty footer.
    ui_theme::styleScreen(screen, 4);
    lv_obj_set_style_pad_row(screen, 4, 0);
    lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);

    rangeDropdown = lv_dropdown_create(screen);
    lv_obj_set_width(rangeDropdown, lv_pct(100));
    lv_obj_set_height(rangeDropdown, 36);
    lv_dropdown_set_symbol(rangeDropdown, LV_SYMBOL_DOWN);
    lv_dropdown_set_dir(rangeDropdown, LV_DIR_BOTTOM);
    lv_obj_set_style_text_font(rangeDropdown, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_add_event_cb(rangeDropdown, rangeChangedCb, LV_EVENT_VALUE_CHANGED, nullptr);

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

    emptyLabel = lv_label_create(screen);
    lv_obj_set_style_text_color(emptyLabel, lv_palette_main(LV_PALETTE_GREY), 0);

    lv_timer_create(refreshCb, kRefreshIntervalMs, nullptr);
    updateChart();
    return screen;
}

} // namespace usage_screen
