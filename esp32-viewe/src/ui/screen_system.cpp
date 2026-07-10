#include "screen_system.h"
#include "../sensors/sensors.h"
#include <cmath>
#include <cstdio>

// This screen shows the *derived* view of the system: raw In/Out/Aux
// readings live on screen_realtime instead. The system here is a simple
// solar setup: In = panel charging the battery, Out = battery feeding the
// load, so In - Out is the net power actually reaching (or leaving) the
// battery. Aux is independent and deliberately left out of that balance.

namespace screen_system {
namespace {

constexpr uint32_t kUpdateIntervalMs = 500; // match sensors::kSampleIntervalMs
constexpr size_t kChartPoints = 60;         // ~30s visible window at 500ms/tick

// --- Summary tab -------------------------------------------------------------
lv_obj_t* summaryLabels[sensors::SENSOR_COUNT] = {nullptr, nullptr, nullptr};
lv_obj_t* summaryToBatteryLabel = nullptr;

// --- Power tab -----------------------------------------------------------------
lv_obj_t* powerStatusLabel = nullptr; // "Charging" / "Discharging"
lv_obj_t* powerWattsLabel = nullptr;  // magnitude of net power
lv_obj_t* powerChart = nullptr;
lv_chart_series_t* powerSeries = nullptr;

// --- Panel tab -------------------------------------------------------------
lv_obj_t* panelPowerLabel = nullptr;
lv_obj_t* panelDutyLabel = nullptr;
lv_obj_t* panelAvailableLabel = nullptr;
lv_obj_t* panelChart = nullptr;
lv_chart_series_t* panelActualSeries = nullptr;
lv_chart_series_t* panelAvailableSeries = nullptr;

lv_timer_t* updateTimer = nullptr;

// The two charts here plot one point per UI tick (the current instantaneous
// value), unlike screen_realtime's charts which replay every sample from
// the sensor ring buffer. That's a deliberate simplification: net battery
// power mixes samples from two independently-clocked sensors, so there's no
// single well-defined per-sample timeline to replay -- "value right now,
// once per tick" is simpler and accurate enough for this trend view.

void updateCb(lv_timer_t*) {
    sensors::Reading readings[sensors::SENSOR_COUNT];
    bool haveReading[sensors::SENSOR_COUNT] = {false, false, false};
    for (uint8_t i = 0; i < sensors::SENSOR_COUNT; i++) {
        haveReading[i] = sensors::getLatest(static_cast<sensors::SensorId>(i), readings[i]);
    }

    // --- Summary: raw numbers for all three, plus what's reaching the battery.
    const char* names[sensors::SENSOR_COUNT] = {"In", "Out", "Aux"};
    for (uint8_t i = 0; i < sensors::SENSOR_COUNT; i++) {
        if (!haveReading[i]) continue;
        char buf[64];
        snprintf(buf, sizeof(buf), "%s: %.2f V  %.2f A  %.1f W",
                 names[i], readings[i].voltage, readings[i].current, readings[i].power);
        lv_label_set_text(summaryLabels[i], buf);
    }

    float netPower = 0;
    bool haveNet = sensors::getNetBatteryPower(netPower);
    if (haveNet) {
        char buf[48];
        snprintf(buf, sizeof(buf), "Sent to battery: %+.1f W", netPower);
        lv_label_set_text(summaryToBatteryLabel, buf);
    }

    // --- Power: charging/discharging indicator + trend.
    if (haveNet) {
        bool charging = netPower >= 0;
        lv_label_set_text(powerStatusLabel, charging ? "Charging" : "Discharging");
        lv_obj_set_style_text_color(
            powerStatusLabel,
            lv_palette_main(charging ? LV_PALETTE_GREEN : LV_PALETTE_RED), 0);

        char buf[32];
        snprintf(buf, sizeof(buf), "%.1f W", std::fabs(netPower));
        lv_label_set_text(powerWattsLabel, buf);

        lv_chart_set_next_value(powerChart, powerSeries, (lv_coord_t)netPower);
    }

    // --- Panel: actual power vs. duty-cycle-corrected available power.
    if (haveReading[sensors::SENSOR_IN]) {
        float panelPower = readings[sensors::SENSOR_IN].power;
        float duty = sensors::getDutyCycle(sensors::SENSOR_IN);
        float available = panelPower;
        sensors::getAvailablePower(sensors::SENSOR_IN, available);

        char buf[32];
        snprintf(buf, sizeof(buf), "Actual: %.1f W", panelPower);
        lv_label_set_text(panelPowerLabel, buf);
        snprintf(buf, sizeof(buf), "Duty: %.0f%%", duty * 100.0f);
        lv_label_set_text(panelDutyLabel, buf);
        snprintf(buf, sizeof(buf), "Available: %.1f W", available);
        lv_label_set_text(panelAvailableLabel, buf);

        lv_chart_set_next_value(panelChart, panelActualSeries, (lv_coord_t)panelPower);
        lv_chart_set_next_value(panelChart, panelAvailableSeries, (lv_coord_t)available);
    }
}

lv_obj_t* createSummaryTab(lv_obj_t* tabParent) {
    lv_obj_t* tab = lv_obj_create(tabParent);
    lv_obj_set_size(tab, lv_pct(100), lv_pct(100));
    lv_obj_set_style_pad_all(tab, 8, 0);
    lv_obj_set_style_border_width(tab, 0, 0);
    lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(tab, 10, 0);

    for (uint8_t i = 0; i < sensors::SENSOR_COUNT; i++) {
        summaryLabels[i] = lv_label_create(tab);
        lv_label_set_text(summaryLabels[i], "--");
    }

    summaryToBatteryLabel = lv_label_create(tab);
    lv_obj_set_style_text_font(summaryToBatteryLabel, &lv_font_montserrat_22, 0);
    lv_label_set_text(summaryToBatteryLabel, "Sent to battery: -- W");

    return tab;
}

lv_obj_t* createPowerTab(lv_obj_t* tabParent) {
    lv_obj_t* tab = lv_obj_create(tabParent);
    lv_obj_set_size(tab, lv_pct(100), lv_pct(100));
    lv_obj_set_style_pad_all(tab, 8, 0);
    lv_obj_set_style_border_width(tab, 0, 0);
    lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(tab, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    powerStatusLabel = lv_label_create(tab);
    lv_obj_set_style_text_font(powerStatusLabel, &lv_font_montserrat_32, 0);
    lv_label_set_text(powerStatusLabel, "--");

    powerWattsLabel = lv_label_create(tab);
    lv_obj_set_style_text_font(powerWattsLabel, &lv_font_montserrat_22, 0);
    lv_label_set_text(powerWattsLabel, "-- W");

    powerChart = lv_chart_create(tab);
    lv_obj_set_size(powerChart, lv_pct(100), lv_pct(65));
    lv_chart_set_type(powerChart, LV_CHART_TYPE_LINE);
    lv_chart_set_update_mode(powerChart, LV_CHART_UPDATE_MODE_SHIFT);
    lv_chart_set_point_count(powerChart, kChartPoints);
    // Net battery power: negative = discharging, positive = charging.
    // Range is a rough default; tune once real panel/load sizing is known.
    lv_chart_set_range(powerChart, LV_CHART_AXIS_PRIMARY_Y, -50, 50);
    powerSeries = lv_chart_add_series(powerChart, lv_palette_main(LV_PALETTE_BLUE),
                                       LV_CHART_AXIS_PRIMARY_Y);

    return tab;
}

lv_obj_t* createPanelTab(lv_obj_t* tabParent) {
    lv_obj_t* tab = lv_obj_create(tabParent);
    lv_obj_set_size(tab, lv_pct(100), lv_pct(100));
    lv_obj_set_style_pad_all(tab, 4, 0);
    lv_obj_set_style_border_width(tab, 0, 0);
    lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);

    lv_obj_t* labelRow = lv_obj_create(tab);
    lv_obj_set_size(labelRow, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_border_width(labelRow, 0, 0);
    lv_obj_set_flex_flow(labelRow, LV_FLEX_FLOW_ROW);

    panelPowerLabel = lv_label_create(labelRow);
    lv_label_set_text(panelPowerLabel, "Actual: -- W");
    panelDutyLabel = lv_label_create(labelRow);
    lv_label_set_text(panelDutyLabel, "Duty: --%");
    panelAvailableLabel = lv_label_create(labelRow);
    lv_label_set_text(panelAvailableLabel, "Available: -- W");

    panelChart = lv_chart_create(tab);
    lv_obj_set_size(panelChart, lv_pct(100), lv_pct(80));
    lv_chart_set_type(panelChart, LV_CHART_TYPE_LINE);
    lv_chart_set_update_mode(panelChart, LV_CHART_UPDATE_MODE_SHIFT);
    lv_chart_set_point_count(panelChart, kChartPoints);
    // Watts; tune once real panel wattage is known.
    lv_chart_set_range(panelChart, LV_CHART_AXIS_PRIMARY_Y, 0, 100);

    // Actual = measured panel power right now. Available = actual / duty
    // cycle, i.e. what the panel could deliver if the charger weren't
    // throttling via PWM. Available >= actual always; the gap between the
    // two lines is the power currently being left uncaptured.
    panelActualSeries = lv_chart_add_series(panelChart, lv_palette_main(LV_PALETTE_BLUE),
                                             LV_CHART_AXIS_PRIMARY_Y);
    panelAvailableSeries = lv_chart_add_series(panelChart, lv_palette_main(LV_PALETTE_ORANGE),
                                                LV_CHART_AXIS_PRIMARY_Y);

    return tab;
}

} // namespace

lv_obj_t* create(lv_obj_t* parent) {
    lv_obj_t* tabview = lv_tabview_create(parent, LV_DIR_TOP, 40);

    lv_obj_t* summaryTabBtn = lv_tabview_add_tab(tabview, "Summary");
    createSummaryTab(summaryTabBtn);

    lv_obj_t* powerTabBtn = lv_tabview_add_tab(tabview, "Power");
    createPowerTab(powerTabBtn);

    lv_obj_t* panelTabBtn = lv_tabview_add_tab(tabview, "Panel");
    createPanelTab(panelTabBtn);

    updateTimer = lv_timer_create(updateCb, kUpdateIntervalMs, nullptr);

    return tabview;
}

} // namespace screen_system
