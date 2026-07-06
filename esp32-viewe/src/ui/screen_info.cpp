#include "screen_info.h"
#include "nav_bar.h"
#include "screen_manager.h"
#include <Arduino.h>
#include <esp_system.h>

namespace screen_info {
namespace {

lv_obj_t* uptimeLabel = nullptr;
lv_obj_t* heapLabel = nullptr;
lv_obj_t* psramLabel = nullptr;
lv_timer_t* updateTimer = nullptr;

lv_obj_t* addRow(lv_obj_t* parent, const char* key, const char* value) {
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(row, 4, 0);
    lv_obj_set_style_border_width(row, 0, 0);

    lv_obj_t* keyLabel = lv_label_create(row);
    lv_label_set_text(keyLabel, key);
    lv_obj_set_style_text_color(keyLabel, lv_palette_main(LV_PALETTE_GREY), 0);
    lv_obj_set_width(keyLabel, lv_pct(45));

    lv_obj_t* valLabel = lv_label_create(row);
    lv_label_set_text(valLabel, value);
    lv_obj_set_width(valLabel, lv_pct(55));

    return valLabel; // caller can hang onto this to update it later
}

const char* resetReasonStr(esp_reset_reason_t reason) {
    switch (reason) {
        case ESP_RST_POWERON:  return "Power-on";
        case ESP_RST_SW:       return "Software";
        case ESP_RST_PANIC:    return "Panic";
        case ESP_RST_INT_WDT:  return "Interrupt WDT";
        case ESP_RST_TASK_WDT: return "Task WDT";
        case ESP_RST_WDT:      return "Other WDT";
        case ESP_RST_BROWNOUT: return "Brownout";
        default:                return "Other";
    }
}

void formatUptime(char* buf, size_t len) {
    uint32_t s = millis() / 1000;
    snprintf(buf, len, "%luh %02lum %02lus",
              (unsigned long)(s / 3600), (unsigned long)((s % 3600) / 60), (unsigned long)(s % 60));
}

void updateCb(lv_timer_t*) {
    char buf[32];

    formatUptime(buf, sizeof(buf));
    lv_label_set_text(uptimeLabel, buf);

    snprintf(buf, sizeof(buf), "%.0f KB", ESP.getFreeHeap() / 1024.0);
    lv_label_set_text(heapLabel, buf);

    snprintf(buf, sizeof(buf), "%.0f KB", ESP.getFreePsram() / 1024.0);
    lv_label_set_text(psramLabel, buf);
}

} // namespace

lv_obj_t* create() {
    lv_obj_t* scr = lv_obj_create(nullptr);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);

    nav_bar::create(scr, ScreenId::Info);

    lv_obj_t* list = lv_obj_create(scr);
    lv_obj_set_size(list, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_border_width(list, 0, 0);

    char buf[48];

    addRow(list, "Build", __DATE__ " " __TIME__);

    snprintf(buf, sizeof(buf), "%d.%d.%d", LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH);
    addRow(list, "LVGL version", buf);

    addRow(list, "ESP-IDF / SDK", ESP.getSdkVersion());

    snprintf(buf, sizeof(buf), "%s rev %d", ESP.getChipModel(), ESP.getChipRevision());
    addRow(list, "Chip", buf);

    snprintf(buf, sizeof(buf), "%d MHz", ESP.getCpuFreqMHz());
    addRow(list, "CPU freq", buf);

    snprintf(buf, sizeof(buf), "%u MB", (unsigned)(ESP.getFlashChipSize() / (1024 * 1024)));
    addRow(list, "Flash size", buf);

    addRow(list, "Reset reason", resetReasonStr(esp_reset_reason()));

    uptimeLabel = addRow(list, "Uptime", "--");
    heapLabel   = addRow(list, "Free heap", "--");
    psramLabel  = addRow(list, "Free PSRAM", "--");

    updateTimer = lv_timer_create(updateCb, 1000, nullptr);
    updateCb(nullptr); // populate immediately instead of waiting 1s

    return scr;
}

} // namespace screen_info
