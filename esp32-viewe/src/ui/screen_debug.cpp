#include "screen_debug.h"

#include <Arduino.h>
#include <LittleFS.h>
#include <esp_system.h>

namespace screen_debug {
namespace {

lv_obj_t* heapLabel = nullptr;
lv_obj_t* psramLabel = nullptr;
lv_obj_t* storageLabel = nullptr;
lv_timer_t* updateTimer = nullptr;

lv_obj_t* addRow(lv_obj_t* parent, const char* key, const char* value) {
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(row, 4, 0);

    lv_obj_t* keyLabel = lv_label_create(row);
    lv_label_set_text(keyLabel, key);
    lv_obj_set_style_text_color(keyLabel, lv_palette_main(LV_PALETTE_GREY), 0);
    lv_obj_set_width(keyLabel, lv_pct(45));

    lv_obj_t* valueLabel = lv_label_create(row);
    lv_label_set_text(valueLabel, value);
    lv_obj_set_width(valueLabel, lv_pct(55));
    return valueLabel;
}

const char* resetReasonStr(esp_reset_reason_t reason) {
    switch (reason) {
        case ESP_RST_POWERON: return "Power-on";
        case ESP_RST_SW: return "Software";
        case ESP_RST_PANIC: return "Panic";
        case ESP_RST_INT_WDT: return "Interrupt WDT";
        case ESP_RST_TASK_WDT: return "Task WDT";
        case ESP_RST_WDT: return "Other WDT";
        case ESP_RST_BROWNOUT: return "Brownout";
        default: return "Other";
    }
}

void updateCb(lv_timer_t*) {
    char buffer[64];
    snprintf(buffer, sizeof(buffer), "%.0f KB", ESP.getFreeHeap() / 1024.0);
    lv_label_set_text(heapLabel, buffer);
    snprintf(buffer, sizeof(buffer), "%.0f KB", ESP.getFreePsram() / 1024.0);
    lv_label_set_text(psramLabel, buffer);

    const size_t total = LittleFS.totalBytes();
    if (total == 0) {
        lv_label_set_text(storageLabel, "Unmounted");
    } else {
        snprintf(buffer, sizeof(buffer), "%zu KB / %zu KB", LittleFS.usedBytes() / 1024, total / 1024);
        lv_label_set_text(storageLabel, buffer);
    }
}

} // namespace

lv_obj_t* create(lv_obj_t* parent) {
    lv_obj_t* screen = lv_obj_create(parent);
    lv_obj_remove_style_all(screen);
    lv_obj_set_size(screen, lv_pct(100), lv_pct(100));
    lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);

    lv_obj_t* list = lv_obj_create(screen);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);

    char buffer[64];
    snprintf(buffer, sizeof(buffer), "%d.%d.%d", LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH);
    addRow(list, "LVGL", buffer);
    addRow(list, "ESP-IDF / SDK", ESP.getSdkVersion());
    snprintf(buffer, sizeof(buffer), "%s rev %d", ESP.getChipModel(), ESP.getChipRevision());
    addRow(list, "Chip", buffer);
    snprintf(buffer, sizeof(buffer), "%d MHz", ESP.getCpuFreqMHz());
    addRow(list, "CPU", buffer);
    snprintf(buffer, sizeof(buffer), "%u MB", (unsigned)(ESP.getFlashChipSize() / (1024 * 1024)));
    addRow(list, "Flash", buffer);
    addRow(list, "Reset", resetReasonStr(esp_reset_reason()));
    heapLabel = addRow(list, "Free heap", "--");
    psramLabel = addRow(list, "Free PSRAM", "--");
    storageLabel = addRow(list, "Data storage", "--");

    updateTimer = lv_timer_create(updateCb, 1000, nullptr);
    updateCb(nullptr);
    return screen;
}

} // namespace screen_debug
