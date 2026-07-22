#pragma once

#include "sensor_source.h"

#include <stdint.h>

#ifndef POWER_METER_ADS1115_ADDRESS
#define POWER_METER_ADS1115_ADDRESS 0x48
#endif

namespace sensors {

struct Ads1115Diagnostics {
    bool ready = false;
    uint8_t address = POWER_METER_ADS1115_ADDRESS;
    uint32_t successfulConversions = 0;
    uint32_t failedConversions = 0;
    uint32_t lockTimeouts = 0;
    uint32_t lastConversionUs = 0;
    uint32_t lastSuccessMs = 0;
    uint32_t consecutiveFailures = 0;
    uint32_t busErrors = 0;
};

Ads1115Diagnostics getAds1115Diagnostics();

} // namespace sensors

class Ads1115SensorSource final : public SensorSource {
public:
    Ads1115SensorSource(uint8_t sensor, uint8_t voltageChannel, uint8_t currentChannel,
                        bool configured = true);

    bool requiresCalibration() const override { return true; }
    bool init() override;
    SensorSample read() override;

private:
    uint8_t sensor_;
    uint8_t voltageChannel_;
    uint8_t currentChannel_;
    bool configured_;
};
