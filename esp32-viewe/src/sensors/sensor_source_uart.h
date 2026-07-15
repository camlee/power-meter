#pragma once

#include "pm1_uart_protocol.h"
#include "sensor_source.h"
#include <cstdint>

// One logical channel of the shared UART PM1 stream. Construct one source for
// each channel index (In=0, Out=1, Aux=2); all instances share a single UART
// receiver and therefore publish one coherent accepted frame.
class UartPm1SensorSource final : public SensorSource {
public:
    explicit UartPm1SensorSource(uint8_t channel) : channel_(channel) {}
    bool init() override;
    SensorSample read() override;

private:
    uint8_t channel_;
};

namespace sensors {

pm1::Diagnostics getUartDiagnostics();
uint32_t getUartLastValidAgeMs();

} // namespace sensors
