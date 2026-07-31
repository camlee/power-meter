#pragma once

#include <cmath>

namespace sensors {

// Inclusive limits for calculation eligibility. These are deliberately
// broader than expected operating values: they reject impossible energy
// inputs without hiding unusual but physically possible observations.
constexpr float kMinimumVoltageV = 0.0f;
constexpr float kMaximumVoltageV = 250.0f;
constexpr float kMinimumCurrentA = -150.0f;
constexpr float kMaximumCurrentA = 150.0f;
constexpr float kMinimumDutyCycle = 0.0f;
constexpr float kMaximumDutyCycle = 1.0f;

inline bool isPlausibleVoltage(float value) {
    return std::isfinite(value) && value >= kMinimumVoltageV && value <= kMaximumVoltageV;
}

inline bool isPlausibleCurrent(float value) {
    return std::isfinite(value) && value >= kMinimumCurrentA && value <= kMaximumCurrentA;
}

inline bool isPlausibleDutyCycle(float value) {
    return std::isfinite(value) && value >= kMinimumDutyCycle && value <= kMaximumDutyCycle;
}

} // namespace sensors
