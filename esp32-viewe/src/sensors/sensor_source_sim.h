#pragma once
#include "sensor_source.h"
#include <cstdint>

// One channel of the shared deterministic Demo scenario schedule. All three
// instances use the same clock so Solar, Load, and Battery remain physically
// coherent while each scenario is active.
class SimulatedSensorSource : public SensorSource {
public:
    explicit SimulatedSensorSource(uint8_t channel);

    bool init() override;
    SensorSample read() override;

private:
    uint8_t channel_;
};
