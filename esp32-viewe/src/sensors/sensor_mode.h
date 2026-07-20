#pragma once
#include <cstdint>

namespace sensor_mode {

// Persisted V1 values. The acquisition source is explicit; ADC and UART both
// belong to the Real history dataset while Demo records to Demo history.
enum class Mode : uint8_t {
    Adc = 0,
    Uart = 1,
    Demo = 2,
};

Mode get();
bool isSupported(Mode mode);
bool set(Mode mode);
const char* label();
} // namespace sensor_mode
