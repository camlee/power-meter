#pragma once

#include <stdint.h>

// Central sensor wiring and source selection. Keep simulation enabled until
// the physical sensor assembly is available for validation. Set this to false
// (or override it with a build flag) to read the ESP32-S3 ADC inputs.
#ifndef POWER_METER_USE_SIMULATED_SENSORS
#define POWER_METER_USE_SIMULATED_SENSORS 1
#endif

namespace sensors::config {

struct Pins {
    uint8_t voltage;
    uint8_t current;
};

// Confirmed VIEWE physical order and wiring. This remains the only place GPIO
// assignments live so acquisition and UI code operate on sensor identities.
constexpr Pins kPins[] = {
    {.voltage = 6, .current = 5},   // Sensor 1: Solar (legacy In)
    {.voltage = 10, .current = 9},  // Sensor 2: Load (legacy Out)
    {.voltage = 8, .current = 7},   // Sensor 3: Battery (legacy Aux)
};

// Factory calibration defaults. Runtime per-channel values are persisted in
// NVS by sensor_calibration and begin with these values on a fresh meter.
constexpr float kVoltageOffsetV = 0.0f;
constexpr float kVoltageVoltsPerInputVolt = 1.0f / 0.027027f;
constexpr float kCurrentOffsetV = 1.667f;
constexpr float kCurrentAmpsPerInputVolt = 1.0f / 0.026667f;

} // namespace sensors::config
