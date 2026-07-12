#pragma once
#include <cstdint>
#include <cstddef>

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
constexpr size_t kHistorySize = 600;   // 5 min of history at 500 ms/sample

// Window used for duty-cycle calculations. Kept separate (and smaller) from
// kHistorySize because it doubles as a fixed-size stack buffer in
// getDutyCycle() -- passing a larger `window` just gets clamped to this.
constexpr size_t kDutyWindowSize = 60; // ~30 s at 500 ms/sample

struct Reading {
    uint32_t timestamp_ms;
    float voltage;
    float current;
    float power;
    // Raw ADC input voltages in Real mode. They are retained for the local
    // calibration preview; Demo mode mirrors its simulated values here.
    float voltageInputV;
    float currentInputV;
    // Direct duty reported by the source, or -1 when it must be inferred.
    float dutyCycle;
};

void start();

// --- Per-sample history -----------------------------------------------------
size_t getRecent(SensorId id, Reading* out, size_t maxCount);
bool getLatest(SensorId id, Reading& out);

// --- Derived values ----------------------------------------------------------
// Duty cycle over the most recent `window` samples: mean power / near-peak
// power, clamped to [0, 1]. 1.0 means the sensor is running flat-out;
// well below 1.0 means (for the panel) the charger has throttled into PWM
// and is leaving power uncaptured. Returns 1.0 if there isn't enough power
// flowing to measure meaningfully (avoids a noisy/undefined ratio near 0).
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
