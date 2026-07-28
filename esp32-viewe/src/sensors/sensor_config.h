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

// Provisional sequential pairing. This is intentionally the only place pin
// assignments live, so the final wiring can be changed without touching the
// acquisition or UI code.
constexpr Pins kPins[] = {
    {.voltage = 6, .current = 5},   // In
    {.voltage = 8, .current = 7},   // Out
    {.voltage = 10, .current = 9},  // Aux
};

// Factory calibration defaults. Runtime per-channel values are persisted in
// NVS by sensor_calibration and begin with these values on a fresh meter.
constexpr float kVoltageOffsetV = 0.0f;
constexpr float kVoltageVoltsPerInputVolt = 1.0f / 0.027027f;
constexpr float kCurrentOffsetV = 1.667f;
constexpr float kCurrentAmpsPerInputVolt = 1.0f / 0.026667f;

} // namespace sensors::config
