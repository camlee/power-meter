#pragma once
#include <cstdint>

namespace sensor_mode {

// Persisted V1 values. The acquisition source is explicit; ADC and UART both
// belong to the Real history dataset while Demo records to Demo history.
enum class Mode : uint8_t {
    // Keep the original persisted values stable. ADS1115 was added after V1
    // and therefore gets a new value rather than reusing the generic ADC slot.
    Adc = 0,
    Uart = 1,
    Demo = 2,
    Ads1115 = 3,
};

Mode get();
bool isSupported(Mode mode);
bool set(Mode mode);
const char* label();
const char* name(Mode mode);
bool usesCalibration(Mode mode);
} // namespace sensor_mode
