#include <Arduino.h>
#include <esp_display_panel.hpp>
#include <lvgl.h>
#include <LittleFS.h>
#include "board_setup.h"
#include "lvgl_v8_port.h"


#include "sensors/sensors.h"
#include "data/historical_storage.h"

#include "ui/screen_manager.h"
#include "ui/screen_realtime.h"
#include "ui/screen_system.h"
#include "ui/screen_historical.h"
#include "ui/screen_wifi.h"
#include "ui/screen_info.h"

using namespace esp_panel::board;
using namespace esp_panel::drivers;

namespace {
uint32_t lastStorageFeedMs = 0;
} // namespace

void setup()
{
    Serial.begin(115200);

    delay(3000);

    sensors::start();
    historical_storage::init();

    Board* board = initDisplayAndLvgl();

    lvgl_port_lock(-1);

    ScreenManager& screens = ScreenManager::instance();
    screens.init(); // Creates the persistent layout

    // Register screens in the order you want them to swipe
    screens.registerScreen("Now", screen_realtime::create);
    screens.registerScreen("Power", screen_system::create);
    screens.registerScreen("Usage", screen_historical::create);
    screens.registerScreen("WiFi", screen_wifi::create);
    screens.registerScreen("Info", screen_info::create);

    screens.build(); // Builds tiles and dots

    lvgl_port_unlock();
}

void loop()
{
    lv_timer_handler();

    uint32_t now = millis();
    if (now - lastStorageFeedMs >= sensors::kSampleIntervalMs) {
        lastStorageFeedMs = now;
        for (uint8_t i = 0; i < sensors::SENSOR_COUNT; i++) {
            sensors::Reading r;
            if (sensors::getLatest(static_cast<sensors::SensorId>(i), r)) {
                historical_storage::addSample(i, r.power, r.timestamp_ms);
            }
        }
    }
    historical_storage::tick(); // cheap; fine to call every loop()

    delay(5);
}
