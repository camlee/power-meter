#include "cycle_screen.h"

#include "../../data/energy_cycle.h"
#include "../../data/history_query_service.h"
#include "../../time/time_service.h"
#include "../components/linear_progress.h"
#include "../theme/ui_theme.h"

#include <Arduino.h>
#include <cmath>
#include <cstdio>
#include <ctime>

namespace cycle_screen {
namespace {
constexpr uint32_t kRefreshMs = 5UL * 60UL * 1000UL;
constexpr uint32_t kCompletionPollMs = 40;
constexpr uint32_t kRetryMs = 30000;
constexpr lv_coord_t kDayWidth = 78;
constexpr lv_coord_t kValueWidth = 69;

struct RowWidgets {
    lv_obj_t* day = nullptr;
    lv_obj_t* warning = nullptr;
    lv_obj_t* charge = nullptr;
    lv_obj_t* use = nullptr;
    lv_obj_t* net = nullptr;
};

lv_obj_t* screenObject = nullptr;
lv_obj_t* endDropdown = nullptr;
lv_obj_t* balanceNet = nullptr;
lv_obj_t* todayNet = nullptr;
lv_obj_t* progress = nullptr;
lv_obj_t* footerWarning = nullptr;
lv_obj_t* statusLabel = nullptr;
RowWidgets rows[energy_cycle::kRecentCycleCount]{};
uint32_t pendingJob = 0;
uint32_t nextRefreshAtMs = 0;

lv_color_t positiveColor() { return lv_color_hex(ui_theme::isDark() ? 0x55C982 : 0x168447); }
lv_color_t negativeColor() { return lv_color_hex(ui_theme::isDark() ? 0xF07A70 : 0xC33B32); }

void setNetColor(lv_obj_t* label, float value, bool available) {
    lv_color_t color = ui_theme::mutedText();
    if (available && value > 0.0f) color = positiveColor();
    else if (available && value < 0.0f) color = negativeColor();
    else if (available) color = ui_theme::text();
    lv_obj_set_style_text_color(label, color, 0);
}

void setEnergy(lv_obj_t* label, float value, bool available, bool signedValue = false) {
    char text[18];
    if (!available || !std::isfinite(value)) lv_snprintf(text, sizeof(text), "--");
    // LVGL's printf configuration does not include floating-point support.
    else if (signedValue) snprintf(text, sizeof(text), "%+.0f", static_cast<double>(value));
    else snprintf(text, sizeof(text), "%.0f", static_cast<double>(value));
    lv_label_set_text(label, text);
}

void formatDay(char* out, size_t size, const energy_cycle::Summary& summary) {
    if (summary.current) {
        lv_snprintf(out, size, "Today");
        return;
    }
    // Label a completed cycle by the local date on which it ended.
    const int64_t localMs = summary.endUnixMs +
        static_cast<int64_t>(time_service::utcOffsetMinutes()) * 60000LL - 1;
    const time_t seconds = static_cast<time_t>(localMs / 1000LL);
    struct tm local{};
    gmtime_r(&seconds, &local);
    char day[8];
    strftime(day, sizeof(day), "%a", &local);
    lv_snprintf(out, size, "%s", day);
}

void clearRows() {
    for (size_t i = 0; i < energy_cycle::kRecentCycleCount; ++i) {
        lv_label_set_text(rows[i].day, i == 0 ? "Today" : "--");
        lv_label_set_text(rows[i].warning, "");
        lv_label_set_text(rows[i].charge, "--");
        lv_label_set_text(rows[i].use, "--");
        lv_label_set_text(rows[i].net, "--");
        lv_obj_set_style_text_color(rows[i].net, ui_theme::mutedText(), 0);
    }
    lv_label_set_text(balanceNet, "-- Wh");
    lv_obj_set_style_text_color(balanceNet, ui_theme::mutedText(), 0);
    lv_label_set_text(todayNet, "-- Wh");
    lv_obj_set_style_text_color(todayNet, ui_theme::mutedText(), 0);
    lv_obj_add_flag(footerWarning, LV_OBJ_FLAG_HIDDEN);
}

void render(const energy_cycle::Summary* summaries, size_t count) {
    clearRows();
    bool anyIncomplete = false;
    bool balanceAvailable = count >= energy_cycle::kRecentCycleCount;
    float balanceWh = 0.0f;
    if (balanceAvailable) {
        for (size_t i = count - energy_cycle::kRecentCycleCount; i < count; ++i) {
            balanceAvailable &= summaries[i].netAvailable;
            balanceWh += summaries[i].netWh;
        }
    }
    for (size_t displayIndex = 0; displayIndex < count && displayIndex < energy_cycle::kRecentCycleCount;
         ++displayIndex) {
        const energy_cycle::Summary& summary = summaries[count - 1 - displayIndex];
        RowWidgets& row = rows[displayIndex];
        char day[24];
        formatDay(day, sizeof(day), summary);
        lv_label_set_text(row.day, day);
        lv_label_set_text(row.warning, summary.incomplete ? LV_SYMBOL_WARNING : "");
        setEnergy(row.charge, summary.chargeWh, summary.chargeAvailable);
        setEnergy(row.use, summary.useWh, summary.useAvailable);
        setEnergy(row.net, summary.netWh, summary.netAvailable, true);
        setNetColor(row.net, summary.netWh, summary.netAvailable);
        anyIncomplete |= summary.incomplete;
    }

    char balanceText[24];
    if (balanceAvailable) snprintf(balanceText, sizeof(balanceText), "%+.0f Wh", static_cast<double>(balanceWh));
    else lv_snprintf(balanceText, sizeof(balanceText), "-- Wh");
    lv_label_set_text(balanceNet, balanceText);
    setNetColor(balanceNet, balanceWh, balanceAvailable);

    if (count) {
        const energy_cycle::Summary& current = summaries[count - 1];
        char text[24];
        if (current.netAvailable) snprintf(text, sizeof(text), "%+.0f Wh", static_cast<double>(current.netWh));
        else lv_snprintf(text, sizeof(text), "-- Wh");
        lv_label_set_text(todayNet, text);
        setNetColor(todayNet, current.netWh, current.netAvailable);
    }
    if (anyIncomplete) lv_obj_clear_flag(footerWarning, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(footerWarning, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(statusLabel, "");
}

void startQuery(bool replacePending = false) {
    if (!screenObject || !lv_obj_is_visible(screenObject)) return;
    if (pendingJob && !replacePending) return;
    pendingJob = history_query_service::requestCycles();
    if (!pendingJob) {
        lv_label_set_text(statusLabel, "History unavailable");
        nextRefreshAtMs = millis() + kRetryMs;
        return;
    }
    linear_progress::show(progress);
}

void completionCb(lv_timer_t*) {
    if (!pendingJob || !screenObject || !lv_obj_is_visible(screenObject)) return;
    energy_cycle::Summary summaries[energy_cycle::kRecentCycleCount]{};
    size_t count = 0;
    if (!history_query_service::takeCycles(pendingJob, summaries,
            energy_cycle::kRecentCycleCount, count)) {
        // The shared worker is newest-request-wins. A browser query can
        // supersede this screen's job; retry once that newer job is finished.
        if (!history_query_service::busy()) {
            pendingJob = 0;
            startQuery();
        }
        return;
    }
    pendingJob = 0;
    linear_progress::hide(progress);
    if (!count) {
        clearRows();
        lv_label_set_text(statusLabel, time_service::hasCurrentTime()
            ? "No cycle data yet" : "Set time to view cycles");
    } else {
        render(summaries, count);
    }
    nextRefreshAtMs = millis() + kRefreshMs;
}

void refreshCb(lv_timer_t*) {
    if (!screenObject || !lv_obj_is_visible(screenObject)) return;
    const uint8_t configuredHour = energy_cycle::endHour();
    if (lv_dropdown_get_selected(endDropdown) != configuredHour) {
        lv_dropdown_set_selected(endDropdown, configuredHour);
        startQuery(true);
        return;
    }
    if (!pendingJob && nextRefreshAtMs && static_cast<int32_t>(millis() - nextRefreshAtMs) >= 0) startQuery();
}

void screenRefreshCb(lv_event_t* event) {
    if (lv_event_get_code(event) != LV_EVENT_REFRESH) return;
    lv_dropdown_set_selected(endDropdown, energy_cycle::endHour());
    startQuery(true);
}

void dropdownCb(lv_event_t* event) {
    if (lv_event_get_code(event) == LV_EVENT_READY) {
        lv_obj_t* list = lv_dropdown_get_list(lv_event_get_target(event));
        lv_obj_set_style_text_font(list, &lv_font_montserrat_16, LV_PART_MAIN);
        lv_obj_set_style_text_line_space(list, 8, LV_PART_MAIN);
        return;
    }
    if (lv_event_get_code(event) != LV_EVENT_VALUE_CHANGED) return;
    const uint16_t hour = lv_dropdown_get_selected(lv_event_get_target(event));
    if (hour < 24 && energy_cycle::setEndHour(static_cast<uint8_t>(hour))) {
        nextRefreshAtMs = 0;
        startQuery(true);
    }
}

lv_obj_t* makeCell(lv_obj_t* parent, lv_coord_t width, const char* text,
                   const lv_font_t* font, lv_text_align_t align) {
    lv_obj_t* label = lv_label_create(parent);
    lv_obj_set_width(label, width);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_align(label, align, 0);
    lv_label_set_text(label, text);
    return label;
}

lv_obj_t* makeTableRow(lv_obj_t* parent, lv_coord_t height) {
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), height);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    return row;
}
} // namespace

lv_obj_t* create(lv_obj_t* parent) {
    lv_obj_t* screen = lv_obj_create(parent);
    screenObject = screen;
    ui_theme::styleScreen(screen, 5);
    lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(screen, 2, 0);

    lv_obj_t* titleRow = lv_obj_create(screen);
    lv_obj_remove_style_all(titleRow);
    lv_obj_set_size(titleRow, lv_pct(100), 38);
    lv_obj_set_flex_flow(titleRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(titleRow, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(titleRow, 6, 0);
    lv_obj_t* title = lv_label_create(titleRow);
    lv_obj_set_width(title, 0);
    lv_obj_set_flex_grow(title, 1);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_label_set_text(title, "Energy Balance");
    endDropdown = lv_dropdown_create(titleRow);
    lv_obj_set_size(endDropdown, 115, 38);
    lv_dropdown_set_options(endDropdown,
        "12:00 AM\n1:00 AM\n2:00 AM\n3:00 AM\n4:00 AM\n5:00 AM\n6:00 AM\n7:00 AM\n"
        "8:00 AM\n9:00 AM\n10:00 AM\n11:00 AM\n12:00 PM\n1:00 PM\n2:00 PM\n3:00 PM\n"
        "4:00 PM\n5:00 PM\n6:00 PM\n7:00 PM\n8:00 PM\n9:00 PM\n10:00 PM\n11:00 PM");
    lv_dropdown_set_selected(endDropdown, energy_cycle::endHour());
    lv_dropdown_set_symbol(endDropdown, LV_SYMBOL_DOWN);
    lv_obj_set_style_text_font(endDropdown, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_add_event_cb(endDropdown, dropdownCb, LV_EVENT_ALL, nullptr);

    lv_obj_t* efficiency = lv_label_create(screen);
    lv_obj_set_width(efficiency, lv_pct(100));
    lv_obj_set_style_text_font(efficiency, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(efficiency, ui_theme::mutedText(), 0);
    lv_obj_set_style_text_align(efficiency, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(efficiency, "Assuming 80% charge efficiency");

    lv_obj_t* summary = lv_obj_create(screen);
    lv_obj_remove_style_all(summary);
    lv_obj_set_size(summary, lv_pct(100), 51);
    lv_obj_set_flex_flow(summary, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(summary, LV_FLEX_ALIGN_SPACE_AROUND, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t* balanceKpi = lv_obj_create(summary);
    lv_obj_remove_style_all(balanceKpi);
    lv_obj_set_size(balanceKpi, 0, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(balanceKpi, 1);
    lv_obj_set_flex_flow(balanceKpi, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(balanceKpi, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t* balanceLabel = lv_label_create(balanceKpi);
    lv_obj_set_style_text_font(balanceLabel, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(balanceLabel, ui_theme::mutedText(), 0);
    lv_label_set_text(balanceLabel, "7-DAY NET");
    balanceNet = lv_label_create(balanceKpi);
    lv_obj_set_style_text_font(balanceNet, &lv_font_montserrat_22, 0);
    lv_label_set_text(balanceNet, "-- Wh");

    lv_obj_t* todayKpi = lv_obj_create(summary);
    lv_obj_remove_style_all(todayKpi);
    lv_obj_set_size(todayKpi, 0, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(todayKpi, 1);
    lv_obj_set_flex_flow(todayKpi, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(todayKpi, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t* todayLabel = lv_label_create(todayKpi);
    lv_obj_set_style_text_font(todayLabel, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(todayLabel, ui_theme::mutedText(), 0);
    lv_label_set_text(todayLabel, "TODAY SO FAR");
    todayNet = lv_label_create(todayKpi);
    lv_obj_set_style_text_font(todayNet, &lv_font_montserrat_22, 0);
    lv_label_set_text(todayNet, "-- Wh");

    lv_obj_t* header = makeTableRow(screen, 23);
    makeCell(header, kDayWidth, "Day", &lv_font_montserrat_14, LV_TEXT_ALIGN_LEFT);
    makeCell(header, kValueWidth, "Charged", &lv_font_montserrat_14, LV_TEXT_ALIGN_RIGHT);
    makeCell(header, kValueWidth, "Used", &lv_font_montserrat_14, LV_TEXT_ALIGN_RIGHT);
    makeCell(header, kValueWidth, "Net", &lv_font_montserrat_14, LV_TEXT_ALIGN_RIGHT);

    for (size_t i = 0; i < energy_cycle::kRecentCycleCount; ++i) {
        lv_obj_t* row = makeTableRow(screen, 33);
        if (i & 1U) {
            lv_obj_set_style_bg_color(row, ui_theme::surfaceAlt(), 0);
            lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(row, 3, 0);
        }
        lv_obj_t* dayCell = lv_obj_create(row);
        lv_obj_remove_style_all(dayCell);
        lv_obj_set_size(dayCell, kDayWidth, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(dayCell, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(dayCell, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(dayCell, 2, 0);
        rows[i].day = lv_label_create(dayCell);
        lv_obj_set_width(rows[i].day, 64);
        lv_obj_set_style_text_font(rows[i].day, &lv_font_montserrat_20, 0);
        lv_label_set_text(rows[i].day, "--");
        // Reserve a fixed warning column so every icon shares the same x
        // position regardless of the weekday label width.
        rows[i].warning = lv_label_create(dayCell);
        lv_obj_set_width(rows[i].warning, 10);
        lv_obj_set_style_text_align(rows[i].warning, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(rows[i].warning, &lv_font_montserrat_10, 0);
        lv_obj_set_style_text_color(rows[i].warning, ui_theme::mutedText(), 0);
        lv_label_set_text(rows[i].warning, "");
        rows[i].charge = makeCell(row, kValueWidth, "--", &lv_font_montserrat_20, LV_TEXT_ALIGN_RIGHT);
        rows[i].use = makeCell(row, kValueWidth, "--", &lv_font_montserrat_20, LV_TEXT_ALIGN_RIGHT);
        rows[i].net = makeCell(row, kValueWidth, "--", &lv_font_montserrat_20, LV_TEXT_ALIGN_RIGHT);
    }

    lv_obj_t* spacer = lv_obj_create(screen);
    lv_obj_remove_style_all(spacer);
    lv_obj_set_width(spacer, 1);
    lv_obj_set_flex_grow(spacer, 1);

    footerWarning = lv_label_create(screen);
    lv_obj_set_width(footerWarning, lv_pct(100));
    lv_obj_set_style_text_font(footerWarning, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(footerWarning, ui_theme::mutedText(), 0);
    lv_obj_set_style_text_align(footerWarning, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(footerWarning, LV_SYMBOL_WARNING " Incomplete Data");
    lv_obj_add_flag(footerWarning, LV_OBJ_FLAG_HIDDEN);

    statusLabel = lv_label_create(screen);
    lv_obj_set_width(statusLabel, lv_pct(100));
    lv_obj_set_style_text_font(statusLabel, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(statusLabel, ui_theme::mutedText(), 0);
    lv_obj_set_style_text_align(statusLabel, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(statusLabel, "");

    progress = linear_progress::create(screen);
    lv_obj_add_event_cb(screen, screenRefreshCb, LV_EVENT_REFRESH, nullptr);
    lv_timer_create(completionCb, kCompletionPollMs, nullptr);
    lv_timer_create(refreshCb, 1000, nullptr);
    clearRows();
    return screen;
}

} // namespace cycle_screen
