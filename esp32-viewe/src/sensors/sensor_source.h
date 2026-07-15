#pragma once
#include <cstdint>
#include <limits>

// Transport/acquisition state reported by a source. Plausibility is applied
// centrally by sensors.cpp after calibration, so individual sources do not
// get to define calculation policy.
enum class SensorSampleState : uint8_t {
    NotConfigured,
    Waiting,
    Observed,
    Invalid,
    Stale,
};

// One electrical sample at read time. In ADC mode these are raw ADC input
// volts; sensors.cpp applies the persisted calibration. Other sources return
// engineering-unit values and bypass calibration.
struct SensorSample {
    SensorSampleState state = SensorSampleState::Waiting;
    // Presence is independent from runtime state. UART remains unconfigured
    // while waiting for its first authoritative mask; a known ADC channel may
    // be configured while Waiting/Invalid.
    bool configured = false;
    float voltage = std::numeric_limits<float>::quiet_NaN();
    float current = std::numeric_limits<float>::quiet_NaN();
    // Sources that can observe sub-sample PWM may report its averaged duty.
    // Keep presence separate from the value: an omitted duty can be derived,
    // while a reported-but-invalid duty remains a data-quality diagnostic.
    bool hasDutyCycle = false;
    float dutyCycle = std::numeric_limits<float>::quiet_NaN();
};

// Implement this once per physical sensor part (e.g. INA219, INA226,
// ADS1115 + shunt...). Nothing outside sensors.cpp ever talks to a
// SensorSource directly, so swapping simulated -> real hardware means:
//   1. Write a new class implementing this interface.
//   2. Change the 3 lines in sensors.cpp that construct SimulatedSensorSource.
// Everything else (buffering, UI, storage) is untouched.
class SensorSource {
public:
    virtual ~SensorSource() = default;

    // ADC sources return input volts and opt into the persisted ESP32
    // calibration. Demo and UART sources already report engineering units and
    // leave this false so their values are never calibrated twice.
    virtual bool requiresCalibration() const { return false; }

    // Called once at startup (e.g. begin I2C, check chip ID). Return false
    // on failure -- sensors.cpp will log it and publish Invalid readings rather
    // than fabricate a zero observation or crash the task.
    virtual bool init() = 0;

    // Called once per sample interval from the sensor task. Should be fast
    // and non-blocking where possible (a quick I2C/SPI transaction is fine).
    virtual SensorSample read() = 0;
};
