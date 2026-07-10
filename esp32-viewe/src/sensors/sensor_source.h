#pragma once
#include <cstdint>

// One electrical sample: raw voltage (V) and current (A) at read time.
// Power is NOT computed here on purpose -- it's derived once, centrally,
// in sensors.cpp, so every SensorSource (real or simulated) only has to
// report the two raw quantities it actually measures.
struct SensorSample {
    float voltage;
    float current;
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

    // Called once at startup (e.g. begin I2C, check chip ID). Return false
    // on failure -- sensors.cpp will log it and keep running with the last
    // known-good reading of 0 rather than crash the task.
    virtual bool init() = 0;

    // Called once per sample interval from the sensor task. Should be fast
    // and non-blocking where possible (a quick I2C/SPI transaction is fine).
    virtual SensorSample read() = 0;
};
