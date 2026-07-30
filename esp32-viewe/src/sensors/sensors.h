#pragma once
#include "sensor_limits.h"
#include <cstdint>
#include <cstddef>
#include <limits>

// Logical roles consumed by power flow, storage, APIs, and UI. Hardware
// sources are independently identified as Sensor 1/2/3 in sensor_mapping.
namespace sensors {

enum SensorId : uint8_t {
    SENSOR_SOLAR = 0,
    SENSOR_LOAD = 1,
    SENSOR_BATTERY = 2,
    SENSOR_COUNT = 3,
    // Stable aliases for persisted/wire formats that still use In/Out/Aux.
    SENSOR_IN = SENSOR_SOLAR,
    SENSOR_OUT = SENSOR_LOAD,
    SENSOR_AUX = SENSOR_BATTERY,
};

constexpr uint32_t kSampleIntervalMs = 500;
// ADC sources sample each logical voltage/current pair at these target
// cadences, then reduce the observations into the normal 500 ms Reading
// stream. Actual cadence is included in every on-demand capture.
constexpr uint32_t kEsp32AdcAcquisitionIntervalMs = 5;
constexpr uint32_t kAds1115AcquisitionIntervalMs = 15;
constexpr uint32_t kFastestAcquisitionIntervalMs =
    kEsp32AdcAcquisitionIntervalMs < kAds1115AcquisitionIntervalMs
        ? kEsp32AdcAcquisitionIntervalMs : kAds1115AcquisitionIntervalMs;
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

// A raw-capture slice covers one selected logical channel. It retains the
// exact calibrated observations that feed three consecutive 500 ms production
// reducers, then transfers ownership to the requesting API.
constexpr uint8_t kAdcCaptureWindowCount = 3;
constexpr size_t kAdcCapturePointCapacity =
    (kSampleIntervalMs / kFastestAcquisitionIntervalMs + 4) * kAdcCaptureWindowCount;
constexpr uint32_t kAdcCaptureActiveTimeoutMs = 10000;
constexpr uint32_t kAdcCaptureReadyTimeoutMs = 30000;

enum class AdcCaptureState : uint8_t {
    Unavailable,
    Idle,
    Armed,
    Capturing,
    Ready,
};

struct AdcCapturePoint {
    uint32_t elapsedUs = 0;
    float voltage = std::numeric_limits<float>::quiet_NaN();
    float current = std::numeric_limits<float>::quiet_NaN();
    float power = std::numeric_limits<float>::quiet_NaN();
};

struct AdcCaptureWindow {
    uint32_t startUs = 0;
    uint32_t endUs = 0;
    uint16_t firstPoint = 0;
    uint16_t pointCount = 0;
    Reading reading{};
};

struct AdcCaptureStatus {
    AdcCaptureState state = AdcCaptureState::Unavailable;
    uint32_t captureId = 0;
    SensorId channel = SENSOR_SOLAR;
    uint16_t pointCount = 0;
    uint8_t windowCount = 0;
    uint8_t targetWindowCount = kAdcCaptureWindowCount;
    uint16_t droppedPoints = 0;
};

struct AdcCaptureResult {
    uint32_t captureId = 0;
    SensorId channel = SENSOR_SOLAR;
    uint32_t requestedIntervalUs = kFastestAcquisitionIntervalMs * 1000;
    uint32_t measuredIntervalUs = 0;
    uint16_t pointCount = 0;
    uint8_t windowCount = 0;
    uint16_t droppedPoints = 0;
    AdcCaptureWindow windows[kAdcCaptureWindowCount]{};
    AdcCapturePoint points[kAdcCapturePointCapacity]{};
};

bool isConfigured(const Reading& reading);
bool isCalculationEligible(const Reading& reading);
bool isDirectDutyEligible(const Reading& reading);
uint8_t getConfiguredMask();

void start();

bool requestAdcCapture(SensorId channel, uint32_t& captureId);
AdcCaptureStatus getAdcCaptureStatus();
// Cancellation and transfer are generation-safe: a stale consumer cannot
// affect a newer request that reused the shared capture service.
bool cancelAdcCapture(uint32_t captureId);
// Copies and releases the completed capture. The service retains no raw
// samples after this succeeds.
bool takeAdcCapture(uint32_t captureId, AdcCaptureResult& out);
const char* adcCaptureStateName(AdcCaptureState state);

// --- Per-sample history -----------------------------------------------------
size_t getRecent(SensorId id, Reading* out, size_t maxCount);
bool getLatest(SensorId id, Reading& out);

// Latest engineering reading for a physical Sensor 1/2/3 channel, after that
// channel's calibration and current-direction transform but before role
// mapping. This is the diagnostics/configuration surface; operational
// consumers should continue using logical getLatest().
bool getLatestPhysical(uint8_t physicalSensor, Reading& out);

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

// Legacy two-channel inferred battery power: Solar minus Load. Positive means
// charging; negative means discharging. This intentionally does not use the
// direct Battery channel. Returns false if either input has no reading yet.
bool getNetBatteryPower(float& outWatts);

// Preferred user-facing system net power. Positive means charging and
// negative means discharging. A mapped, valid Battery is authoritative and
// follows the configured positive-while-charging convention. If Battery is
// unmapped or unavailable, Solar minus Load is used.
enum class NetPowerSource : uint8_t {
    Battery,
    SolarLoadFallback,
};
bool getSystemNetPower(float& outWatts, NetPowerSource* source = nullptr);

} // namespace sensors
