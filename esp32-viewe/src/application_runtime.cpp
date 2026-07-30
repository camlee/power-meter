#include "application_runtime.h"

#include <Arduino.h>
#include <algorithm>
#include <cmath>
#include <esp_heap_caps.h>

#include "data/energy_cycle.h"
#include "data/historical_storage.h"
#include "data/history_query_service.h"
#include "device/hardware_profile.h"
#include "device/i2c_bus.h"
#include "device/status_display.h"
#include "network/live_websocket_service.h"
#include "network/internet_update_service.h"
#include "network/network_manager.h"
#include "network/ota_service.h"
#include "sensors/sensors.h"

void ensureRollbackVerificationDeferral();

namespace application_runtime {
namespace {

uint32_t lastStorageFeedMs = 0;
uint32_t lastNetworkUpdateMs = 0;

historical_storage::SampleFrame makeHistoryFrame(uint32_t now) {
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

    const bool haveSolar = present[sensors::SENSOR_SOLAR] &&
                           sensors::isCalculationEligible(readings[sensors::SENSOR_SOLAR]);
    const bool haveLoad = present[sensors::SENSOR_LOAD] &&
                          sensors::isCalculationEligible(readings[sensors::SENSOR_LOAD]);
    const bool haveBattery = present[sensors::SENSOR_BATTERY] &&
                             sensors::isCalculationEligible(readings[sensors::SENSOR_BATTERY]);
    if (haveSolar && haveLoad) {
        const float solar = readings[sensors::SENSOR_SOLAR].power;
        const float load = readings[sensors::SENSOR_LOAD].power;
        const float net = solar - load;
        const float panelToLoad =
            std::min(std::max(solar, 0.0f), std::max(load, 0.0f));
        frame.componentPowerW[historical_storage::PANEL_IN] = panelToLoad;
        frame.componentPowerW[historical_storage::PANEL_USAGE] = panelToLoad;
        frame.eligibleComponentMask |=
            static_cast<uint8_t>((1U << historical_storage::PANEL_IN) |
                                 (1U << historical_storage::PANEL_USAGE));
        if (hardware_profile::kControllerIsPwm || !haveBattery) {
            frame.componentPowerW[historical_storage::BATTERY_CHARGING] = std::max(net, 0.0f);
            frame.componentPowerW[historical_storage::BATTERY_USAGE] = std::max(-net, 0.0f);
            frame.eligibleComponentMask |=
                static_cast<uint8_t>((1U << historical_storage::BATTERY_CHARGING) |
                                     (1U << historical_storage::BATTERY_USAGE));
        }
    }

    if (!hardware_profile::kControllerIsPwm && haveBattery) {
        const float battery = readings[sensors::SENSOR_BATTERY].power;
        frame.componentPowerW[historical_storage::BATTERY_CHARGING] =
            std::max(battery, 0.0f);
        frame.componentPowerW[historical_storage::BATTERY_USAGE] =
            std::max(-battery, 0.0f);
        frame.eligibleComponentMask |=
            static_cast<uint8_t>((1U << historical_storage::BATTERY_CHARGING) |
                                 (1U << historical_storage::BATTERY_USAGE));
    }

    if (haveSolar) {
        float availableSolarPowerW = 0.0f;
        if (sensors::getAvailablePower(
                sensors::SENSOR_SOLAR, availableSolarPowerW) &&
            std::isfinite(availableSolarPowerW)) {
            frame.componentPowerW[historical_storage::PANEL_SURPLUS] =
                std::max(
                    availableSolarPowerW -
                        readings[sensors::SENSOR_SOLAR].power,
                    0.0f);
            frame.eligibleComponentMask |=
                static_cast<uint8_t>(1U << historical_storage::PANEL_SURPLUS);
        } else if (readings[sensors::SENSOR_SOLAR].dutyState == sensors::DutyState::Invalid) {
            frame.qualityFlags |= historical_storage::QUALITY_REJECTED;
        }
    }
    return frame;
}

} // namespace

void begin() {
    ensureRollbackVerificationDeferral();
    const bool i2cReady = i2c_bus::begin();
    const bool statusDisplayReady = status_display::begin();
    sensors::start();
    const bool storageReady = historical_storage::init();
    energy_cycle::init();
    const bool historyQueryReady = history_query_service::init();
    network_manager::init();
    // The server binds before an interface is available and becomes reachable
    // as soon as station or AP networking comes up.
    ota_service::begin();
    internet_update_service::begin();
    const bool liveReady = live_websocket_service::begin();

    constexpr uint32_t kInternalCaps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    Serial.printf(
        "POWER_METER profile=%s touch=%u status_display=%u esp32_adc=%u ads1115=%u "
        "i2c=%u status_display_ready=%u "
        "storage=%u history_query=%u web=%u live=%u heap_free=%u heap_largest=%u\n",
        hardware_profile::kName, hardware_profile::kHasTouchUi ? 1U : 0U,
        hardware_profile::kHasStatusDisplay ? 1U : 0U,
        hardware_profile::kHasEsp32Adc ? 1U : 0U,
        hardware_profile::kHasAds1115 ? 1U : 0U,
        i2cReady ? 1U : 0U, statusDisplayReady ? 1U : 0U,
        storageReady ? 1U : 0U,
        historyQueryReady ? 1U : 0U, ota_service::isRunning() ? 1U : 0U,
        liveReady ? 1U : 0U,
        static_cast<unsigned>(heap_caps_get_free_size(kInternalCaps)),
        static_cast<unsigned>(heap_caps_get_largest_free_block(kInternalCaps)));
}

void setReady() { ota_service::setApplicationReady(); }

void update() {
    const uint32_t now = millis();
    if (now - lastNetworkUpdateMs >= 200) {
        lastNetworkUpdateMs = now;
        network_manager::update();
    }
    internet_update_service::update();
    ota_service::update();
    live_websocket_service::update();
    i2c_bus::update();
    status_display::update();

    if (now - lastStorageFeedMs >= sensors::kSampleIntervalMs) {
        lastStorageFeedMs = now;
        historical_storage::addSampleFrame(historical_storage::activeDataset(),
                                           makeHistoryFrame(now));
    }
    historical_storage::tick();
    ota_service::noteHealthyLoop();
}

} // namespace application_runtime
