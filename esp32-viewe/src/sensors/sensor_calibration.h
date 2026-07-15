#pragma once

#include "sensor_limits.h"
#include <stdint.h>

namespace sensors::calibration {

enum class Measurement : uint8_t { Voltage = 0, Current = 1 };

// Engineering value = (ADC input volts - offsetInputV) * gain.
// Keeping the offset in ADC volts matches the electrical behaviour of the
// divider/current-sensor circuitry and the original hard-coded conversion.
struct Value {
    float gain;
    float offsetInputV;
};

constexpr float kAdcMinInputV = 0.0f;
constexpr float kAdcMaxInputV = 3.3f;
constexpr float kVoltageMaxV = kMaximumVoltageV;
constexpr float kCurrentMaxA = kMaximumCurrentA;

void init();
Value defaults(Measurement measurement);
Value get(uint8_t sensor, Measurement measurement);
bool set(uint8_t sensor, Measurement measurement, Value value);
bool isValid(Measurement measurement, Value value);
float apply(float inputV, Value value);

} // namespace sensors::calibration
