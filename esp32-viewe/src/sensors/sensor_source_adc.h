#pragma once

#include "sensor_source.h"

#include <stdint.h>

class Esp32AnalogSource final : public SensorSource {
public:
    Esp32AnalogSource(uint8_t voltagePin, uint8_t currentPin);

    bool init() override;
    SensorSample read() override;

private:
    uint8_t voltagePin_;
    uint8_t currentPin_;
};
