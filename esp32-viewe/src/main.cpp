#include <Arduino.h>
#include <esp_display_panel.hpp>
#include <lvgl.h>
#include <LittleFS.h>
#include "board_setup.h"
#include "lvgl_v8_port.h"


#include "sensors/sensors.h"
#include "data/historical_storage.h"
#include "data/history_query_service.h"
#include "network/network_manager.h"
#include "network/ota_service.h"

#include "ui/navigation/app_navigation.h"

using namespace esp_panel::board;
using namespace esp_panel::drivers;

void ensureRollbackVerificationDeferral();

namespace {
uint32_t lastStorageFeedMs = 0;
uint32_t lastNetworkUpdateMs = 0;
} // namespace

void setup()
{
    ensureRollbackVerificationDeferral();
    Serial.begin(115200);

    delay(3000);

    sensors::start();
    historical_storage::init();
    history_query_service::init();
    network_manager::init();
    // The server binds before an interface is available and becomes reachable
    // as soon as station or AP networking comes up.
    ota_service::begin();

    initDisplayAndLvgl();

    lvgl_port_lock(-1);

    ui_navigation::build();

    lvgl_port_unlock();
    ota_service::setApplicationReady();
}

void loop()
{
    uint32_t now = millis();
    if (now - lastNetworkUpdateMs >= 200) {
        lastNetworkUpdateMs = now;
        network_manager::update();
    }
    ota_service::update();

    if (now - lastStorageFeedMs >= sensors::kSampleIntervalMs) {
        lastStorageFeedMs = now;
        sensors::Reading readings[sensors::SENSOR_COUNT];
        bool haveReadings = true;
        for (uint8_t i = 0; i < sensors::SENSOR_COUNT; i++) {
            haveReadings &= sensors::getLatest(static_cast<sensors::SensorId>(i), readings[i]);
        }
        if (haveReadings) {
            float availableInPowerW = readings[sensors::SENSOR_IN].power;
            sensors::getAvailablePower(sensors::SENSOR_IN, availableInPowerW);
            historical_storage::addSampleFrame(
                readings[sensors::SENSOR_IN].power,
                readings[sensors::SENSOR_OUT].power,
                readings[sensors::SENSOR_AUX].power,
                availableInPowerW,
                now);
        }
    }
    historical_storage::tick(); // cheap; fine to call every loop()
    ota_service::noteHealthyLoop();

    delay(5);
}
