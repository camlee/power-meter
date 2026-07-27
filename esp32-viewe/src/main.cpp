#include <Arduino.h>
#include <esp_heap_caps.h>

#include "application_runtime.h"
#include "device/hardware_profile.h"

#if POWER_METER_HAS_TOUCH_UI
#include <cstring>

#include "board_setup.h"
#include "lvgl_v8_port.h"
#include "network/ota_service.h"
#include "ui/navigation/app_navigation.h"
#include "ui/theme/ui_theme.h"

namespace {
uint32_t lastThemeCheckMs = 0;
} // namespace
#endif

void setup() {
#if POWER_METER_HAS_PSRAM
    if (psramFound()) {
        // Preserve scarce DMA-capable internal RAM for Wi-Fi, hardware crypto,
        // and flash operations. The framework default keeps allocations up to
        // 4 KiB internal, which is too aggressive for this UI-heavy target.
        heap_caps_malloc_extmem_enable(512);
    }
#endif
    Serial.begin(115200);
    delay(3000);

    application_runtime::begin();

#if POWER_METER_HAS_TOUCH_UI
    initDisplayAndLvgl();
    lvgl_port_lock(-1);
    ui_navigation::build();
    lvgl_port_unlock();
#endif

    application_runtime::setReady();
}

void loop() {
    application_runtime::update();

#if POWER_METER_HAS_TOUCH_UI
    const uint32_t now = millis();
    if (now - lastThemeCheckMs >= 1000) {
        lastThemeCheckMs = now;
        const bool otaRestartSafe =
            strcmp(ota_service::runningImageState(), "pending_verify") != 0;
        if (ui_theme::autoRestartRequired() && otaRestartSafe) {
            Serial.println("ui_theme: Auto appearance changed; restarting to rebuild LVGL");
            delay(50);
            ESP.restart();
        }
    }
#endif

    delay(5);
}
