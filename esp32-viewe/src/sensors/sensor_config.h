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
    {.voltage = 5, .current = 6},   // In
    {.voltage = 7, .current = 8},   // Out
    {.voltage = 9, .current = 10},  // Aux
};

constexpr float kVoltageOffsetV = 0.0f;
constexpr float kVoltageVoltsPerInputVolt = 1.0f / 0.027027f;
constexpr float kCurrentOffsetV = 1.667f;
constexpr float kCurrentAmpsPerInputVolt = 1.0f / 0.026667f;

} // namespace sensors::config
