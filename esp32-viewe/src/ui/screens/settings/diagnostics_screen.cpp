#include "diagnostics_screen.h"

#include <Arduino.h>
#include <LittleFS.h>
#include <cstring>
#include <esp_system.h>
#include "network/ota_service.h"
#include "../../theme/ui_theme.h"

namespace diagnostics_screen {
namespace {

lv_obj_t* memoryLabel = nullptr;
lv_obj_t* storageLabel = nullptr;
lv_obj_t* otaHealthLabel = nullptr;
lv_obj_t* otaSlotLabel = nullptr;
lv_obj_t* otaStateLabel = nullptr;
lv_timer_t* updateTimer = nullptr;
uint8_t rowIndex = 0;

lv_obj_t* addRow(lv_obj_t* parent, const char* key, const char* value) {
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_hor(row, 6, 0);
    lv_obj_set_style_pad_ver(row, 3, 0);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    if (rowIndex++ & 1U) {
        lv_obj_set_style_bg_color(row, ui_theme::surfaceAlt(), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    }

    lv_obj_t* keyLabel = lv_label_create(row);
    lv_label_set_text(keyLabel, key);
    lv_obj_set_style_text_color(keyLabel, ui_theme::mutedText(), 0);
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
    snprintf(buffer, sizeof(buffer), "Heap %.0f KB  PSRAM %.0f KB",
             ESP.getFreeHeap() / 1024.0, ESP.getFreePsram() / 1024.0);
    lv_label_set_text(memoryLabel, buffer);

    const size_t total = LittleFS.totalBytes();
    if (total == 0) {
        lv_label_set_text(storageLabel, "Unmounted");
    } else {
        snprintf(buffer, sizeof(buffer), "%zu KB / %zu KB", LittleFS.usedBytes() / 1024, total / 1024);
        lv_label_set_text(storageLabel, buffer);
    }

    const uint32_t remainingMs = ota_service::validationRemainingMs();
    if (remainingMs > 0) {
        snprintf(buffer, sizeof(buffer), "%s; verify %.1f s", ota_service::healthStatus(), remainingMs / 1000.0f);
    } else if (strcmp(ota_service::healthStatus(), "confirmed") == 0) {
        snprintf(buffer, sizeof(buffer), "%s; verified", ota_service::healthStatus());
    } else {
        snprintf(buffer, sizeof(buffer), "%s", ota_service::healthStatus());
    }
    lv_label_set_text(otaHealthLabel, buffer);
    snprintf(buffer, sizeof(buffer), "%s -> %s", ota_service::runningPartitionLabel(), ota_service::bootPartitionLabel());
    lv_label_set_text(otaSlotLabel, buffer);
    snprintf(buffer, sizeof(buffer), "%s%s", ota_service::runningImageState(),
             ota_service::rollbackDetected() ? "; rollback detected" : "");
    lv_label_set_text(otaStateLabel, buffer);
}

} // namespace

lv_obj_t* create(lv_obj_t* parent) {
    lv_obj_t* screen = lv_obj_create(parent);
    ui_theme::styleScreen(screen, 4);
    lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);

    lv_obj_t* list = lv_obj_create(screen);
    ui_theme::styleCard(list, 2);
    lv_obj_set_size(list, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 0, 0);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);

    rowIndex = 0;
    char buffer[64];
    snprintf(buffer, sizeof(buffer), "%d.%d.%d", LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH);
    addRow(list, "LVGL", buffer);
    addRow(list, "ESP-IDF / SDK", ESP.getSdkVersion());
    snprintf(buffer, sizeof(buffer), "%s rev %d", ESP.getChipModel(), ESP.getChipRevision());
    addRow(list, "Chip", buffer);
    snprintf(buffer, sizeof(buffer), "%d MHz / %u MB flash", ESP.getCpuFreqMHz(),
             (unsigned)(ESP.getFlashChipSize() / (1024 * 1024)));
    addRow(list, "CPU / flash", buffer);
    addRow(list, "Last reset", resetReasonStr(esp_reset_reason()));
    memoryLabel = addRow(list, "Memory", "--");
    storageLabel = addRow(list, "Data storage", "--");
    otaHealthLabel = addRow(list, "OTA", "--");
    otaSlotLabel = addRow(list, "OTA slots", "--");
    otaStateLabel = addRow(list, "OTA image", "--");

    updateTimer = lv_timer_create(updateCb, 1000, nullptr);
    updateCb(nullptr);
    return screen;
}

} // namespace diagnostics_screen
