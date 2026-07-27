#include "diagnostics_screen.h"

#include <Arduino.h>
#include <LittleFS.h>
#include <cstring>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include "build_time.h"
#include "data/history_query_service.h"
#include "lvgl_v8_port.h"
#include "network/live_websocket_service.h"
#include "network/ota_service.h"
#include "network/web_assets.generated.h"
#include "time/time_service.h"
#include "../../theme/ui_theme.h"

namespace diagnostics_screen {
namespace {

lv_obj_t* internalMemoryLabel = nullptr;
lv_obj_t* psramMemoryLabel = nullptr;
lv_obj_t* lvglStackLabel = nullptr;
lv_obj_t* storageLabel = nullptr;
lv_obj_t* otaHealthLabel = nullptr;
lv_obj_t* otaSlotLabel = nullptr;
lv_obj_t* otaStateLabel = nullptr;
lv_obj_t* historyQueryLabel = nullptr;
lv_obj_t* timeSourceLabel = nullptr;
lv_obj_t* wsConnectionsLabel = nullptr;
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

void updateCb(lv_timer_t* timer) {
    if (timer && timer->user_data && !lv_obj_is_visible(static_cast<lv_obj_t*>(timer->user_data))) return;
    char buffer[64];
    // INTERNAL and SPIRAM select disjoint dynamic heaps. Do not add DEFAULT or
    // DMA figures here: those capabilities overlap these regions and would
    // make a combined percentage misleading. Static data is not part of these
    // heap totals.
    constexpr uint32_t kInternalCaps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    constexpr uint32_t kPsramCaps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    const size_t internalTotal = heap_caps_get_total_size(kInternalCaps);
    const size_t internalFree = heap_caps_get_free_size(kInternalCaps);
    const size_t internalLargest = heap_caps_get_largest_free_block(kInternalCaps);
    const size_t psramTotal = heap_caps_get_total_size(kPsramCaps);
    const size_t psramFree = heap_caps_get_free_size(kPsramCaps);
    const size_t psramLargest = heap_caps_get_largest_free_block(kPsramCaps);
    const unsigned internalUsedPercent = internalTotal
        ? static_cast<unsigned>(((internalTotal - internalFree) * 100 + internalTotal / 2) / internalTotal) : 0;
    const unsigned psramUsedPercent = psramTotal
        ? static_cast<unsigned>(((psramTotal - psramFree) * 100 + psramTotal / 2) / psramTotal) : 0;
    snprintf(buffer, sizeof(buffer), "%u%% used; max %uK", internalUsedPercent,
             static_cast<unsigned>(internalLargest / 1024));
    lv_label_set_text(internalMemoryLabel, buffer);
    snprintf(buffer, sizeof(buffer), "%u%% used; max %uK", psramUsedPercent,
             static_cast<unsigned>(psramLargest / 1024));
    lv_label_set_text(psramMemoryLabel, buffer);
    snprintf(buffer, sizeof(buffer), "min %u B free / %u B",
             static_cast<unsigned>(lvgl_port_stack_minimum_free_bytes()),
             static_cast<unsigned>(lvgl_port_stack_size_bytes()));
    lv_label_set_text(lvglStackLabel, buffer);

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

    time_service::Anchor anchor{};
    lv_label_set_text(timeSourceLabel, time_service::getCurrentAnchor(anchor)
        ? time_service::sourceName(anchor.source) : "unanchored");
    snprintf(buffer, sizeof(buffer), "%u / %u",
             static_cast<unsigned>(live_websocket_service::clientCount()),
             static_cast<unsigned>(live_websocket_service::clientLimit()));
    lv_label_set_text(wsConnectionsLabel, buffer);

    history_query_service::Timing queryTiming{};
    history_query_service::getTiming(queryTiming);
    if (!queryTiming.lastDurationMs) {
        lv_label_set_text(historyQueryLabel, "No query yet");
    } else {
        snprintf(buffer, sizeof(buffer), "%s %lums; %u files, %lu rows (max %lums)",
                 queryTiming.lastWasUsage ? "Usage" : "Files",
                 static_cast<unsigned long>(queryTiming.lastDurationMs), queryTiming.lastFilesRead,
                 static_cast<unsigned long>(queryTiming.lastRecordsRead),
                 static_cast<unsigned long>(queryTiming.maxDurationMs));
        lv_label_set_text(historyQueryLabel, buffer);
    }
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
    internalMemoryLabel = addRow(list, "Internal heap", "--");
    psramMemoryLabel = addRow(list, "PSRAM heap", "--");
    lvglStackLabel = addRow(list, "LVGL stack", "--");
    timeSourceLabel = addRow(list, "Time source", "--");
    addRow(list, "Web build", web_assets::kBuildId);
    storageLabel = addRow(list, "Data storage", "--");
    wsConnectionsLabel = addRow(list, "WS connections", "--");
    otaHealthLabel = addRow(list, "OTA", "--");
    otaSlotLabel = addRow(list, "OTA slots", "--");
    otaStateLabel = addRow(list, "OTA image", "--");
    historyQueryLabel = addRow(list, "History query", "--");

    updateTimer = lv_timer_create(updateCb, 1000, screen);
    if (lv_obj_is_visible(screen)) updateCb(nullptr);
    return screen;
}

} // namespace diagnostics_screen
