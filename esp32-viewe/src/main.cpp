#include <Arduino.h>
#include <esp_display_panel.hpp>
#include <lvgl.h>
#include <LittleFS.h>
#include <algorithm>
#include <cmath>
#include "board_setup.h"
#include "lvgl_v8_port.h"


#include "sensors/sensors.h"
#include "data/historical_storage.h"
#include "data/history_query_service.h"
#include "network/network_manager.h"
#include "network/ota_service.h"
#include "network/live_websocket_service.h"

#include "ui/navigation/app_navigation.h"

using namespace esp_panel::board;
using namespace esp_panel::drivers;

void ensureRollbackVerificationDeferral();

namespace {
uint32_t lastStorageFeedMs = 0;
uint32_t lastNetworkUpdateMs = 0;

historical_storage::SampleFrame makeHistoryFrame(uint32_t now)
{
    historical_storage::SampleFrame frame{};
    frame.timestampMs = now;
    sensors::Reading readings[sensors::SENSOR_COUNT];
    bool present[sensors::SENSOR_COUNT]{};

    for (uint8_t i = 0; i < sensors::SENSOR_COUNT; ++i) {
        present[i] = sensors::getLatest(static_cast<sensors::SensorId>(i), readings[i]);
        if (!present[i] || !sensors::isConfigured(readings[i])) continue;
        frame.configuredChannelMask |= static_cast<uint8_t>(1U << i);
        if (sensors::isCalculationEligible(readings[i])) {
            frame.channelPowerW[i] = readings[i].power;
            frame.eligibleChannelMask |= static_cast<uint8_t>(1U << i);
        } else if (readings[i].state == sensors::ReadingState::Invalid ||
                   readings[i].state == sensors::ReadingState::OutOfRange) {
            frame.qualityFlags |= historical_storage::QUALITY_REJECTED;
        } else if (readings[i].state == sensors::ReadingState::Waiting ||
                   readings[i].state == sensors::ReadingState::Stale) {
            frame.qualityFlags |= historical_storage::QUALITY_STALE_OR_MISSING;
        }
    }

    const bool haveIn = present[sensors::SENSOR_IN] &&
                        sensors::isCalculationEligible(readings[sensors::SENSOR_IN]);
    const bool haveOut = present[sensors::SENSOR_OUT] &&
                         sensors::isCalculationEligible(readings[sensors::SENSOR_OUT]);
    if (haveIn && haveOut) {
        const float in = readings[sensors::SENSOR_IN].power;
        const float out = readings[sensors::SENSOR_OUT].power;
        const float net = in - out;
        const float panelToLoad = std::min(std::max(in, 0.0f), std::max(out, 0.0f));
        frame.componentPowerW[historical_storage::BATTERY_CHARGING] = std::max(net, 0.0f);
        frame.componentPowerW[historical_storage::BATTERY_USAGE] = std::max(-net, 0.0f);
        frame.componentPowerW[historical_storage::PANEL_IN] = panelToLoad;
        frame.componentPowerW[historical_storage::PANEL_USAGE] = panelToLoad;
        frame.eligibleComponentMask |=
            static_cast<uint8_t>((1U << historical_storage::BATTERY_CHARGING) |
                                 (1U << historical_storage::BATTERY_USAGE) |
                                 (1U << historical_storage::PANEL_IN) |
                                 (1U << historical_storage::PANEL_USAGE));
    }

    if (haveIn) {
        float availableInPowerW = 0.0f;
        if (sensors::getAvailablePower(sensors::SENSOR_IN, availableInPowerW) &&
            std::isfinite(availableInPowerW)) {
            frame.componentPowerW[historical_storage::PANEL_SURPLUS] =
                std::max(availableInPowerW - readings[sensors::SENSOR_IN].power, 0.0f);
            frame.eligibleComponentMask |=
                static_cast<uint8_t>(1U << historical_storage::PANEL_SURPLUS);
        } else if (readings[sensors::SENSOR_IN].dutyState == sensors::DutyState::Invalid) {
            frame.qualityFlags |= historical_storage::QUALITY_REJECTED;
        }
    }
    return frame;
}
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
    live_websocket_service::begin();

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
    live_websocket_service::update();

    if (now - lastStorageFeedMs >= sensors::kSampleIntervalMs) {
        lastStorageFeedMs = now;
        historical_storage::addSampleFrame(historical_storage::activeDataset(),
                                           makeHistoryFrame(now));
    }
    historical_storage::tick(); // cheap; fine to call every loop()
    ota_service::noteHealthyLoop();

    delay(5);
}
