#pragma once
#include "sensor_limits.h"
#include <cstdint>
#include <cstddef>
#include <limits>

// Sensor roles in this simple solar system:
//   SENSOR_IN  -- panel feeding the battery
//   SENSOR_OUT -- battery feeding the load
//   SENSOR_AUX -- independent sensor, NOT part of the panel/battery/load
//                 balance (excluded from getNetBatteryPower)
namespace sensors {

enum SensorId : uint8_t {
    SENSOR_IN = 0,
    SENSOR_OUT = 1,
    SENSOR_AUX = 2,
    SENSOR_COUNT
};

constexpr uint32_t kSampleIntervalMs = 500;
#ifndef POWER_METER_SENSOR_HISTORY_SIZE
#define POWER_METER_SENSOR_HISTORY_SIZE 600
#endif
constexpr size_t kHistorySize = POWER_METER_SENSOR_HISTORY_SIZE;

// Window used for duty-cycle calculations. Kept separate (and smaller) from
// kHistorySize because it doubles as a fixed-size stack buffer in
// getDutyCycle() -- passing a larger `window` just gets clamped to this.
constexpr size_t kDutyWindowSize = 60; // ~30 s at 500 ms/sample
static_assert(kHistorySize >= kDutyWindowSize,
              "sensor history must retain the complete duty window");

enum class ReadingState : uint8_t {
    NotConfigured,
    Waiting,
    Valid,
    OutOfRange,
    Invalid,
    Stale,
};

enum class DutyState : uint8_t {
    NotReported,
    Valid,
    Invalid,
};

struct Reading {
    uint32_t timestamp_ms = 0;
    float voltage = std::numeric_limits<float>::quiet_NaN();
    float current = std::numeric_limits<float>::quiet_NaN();
    // The observed voltage/current product when finite, including for an
    // OutOfRange reading. Consumers must check isCalculationEligible() before
    // using it for operational KPIs, derived values, or history.
    float power = std::numeric_limits<float>::quiet_NaN();
    // Raw ADC input voltages in ADC mode. They are retained for the local
    // calibration preview; Demo mirrors its simulated values here.
    float voltageInputV = std::numeric_limits<float>::quiet_NaN();
    float currentInputV = std::numeric_limits<float>::quiet_NaN();
    bool configured = false;
    ReadingState state = ReadingState::Waiting;
    DutyState dutyState = DutyState::NotReported;
    float dutyCycle = std::numeric_limits<float>::quiet_NaN();
};

bool isConfigured(const Reading& reading);
bool isCalculationEligible(const Reading& reading);
bool isDirectDutyEligible(const Reading& reading);
uint8_t getConfiguredMask();

void start();

// --- Per-sample history -----------------------------------------------------
size_t getRecent(SensorId id, Reading* out, size_t maxCount);
bool getLatest(SensorId id, Reading& out);

// --- Derived values ----------------------------------------------------------
// Duty cycle over the most recent `window` samples: mean power / near-peak
// power, clamped to [0, 1]. 1.0 means the sensor is running flat-out;
// well below 1.0 means (for the panel) the charger has throttled into PWM
// and is leaving power uncaptured. Returns 1.0 if there isn't enough power
// flowing to measure meaningfully (avoids a noisy/undefined ratio near 0),
// and NaN when the source explicitly reported an invalid direct duty value.
float getDutyCycle(SensorId id, size_t window = kDutyWindowSize);

// Theoretical power available if duty cycle were 1.0: latest power / duty
// cycle. E.g. 10 W measured at a 0.5 duty cycle -> 20 W available.
// Returns false if there's no reading yet for `id`.
bool getAvailablePower(SensorId id, float& outWatts);

// SENSOR_IN power minus SENSOR_OUT power. Positive == net energy flowing
// into the battery (charging); negative == net draw from the battery
// (discharging). SENSOR_AUX is intentionally excluded from this balance.
// Returns false if either sensor has no reading yet.
bool getNetBatteryPower(float& outWatts);

} // namespace sensors
