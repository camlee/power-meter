#pragma once

#include "pm1_uart_protocol.h"
#include "sensor_source.h"
#include <cstdint>

// One physical channel of the shared UART PM1 stream. Construct one source for
// each producer channel index; the mapping layer assigns those channels to
// Solar/Load/Battery. All instances share one coherent accepted frame.
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
