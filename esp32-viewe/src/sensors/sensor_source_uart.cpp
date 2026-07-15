#include "sensor_source_uart.h"

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace {

constexpr uint32_t kUartBaud = 9600;
constexpr int8_t kUartRxPin = 44;
constexpr int8_t kNoTxPin = -1;

class UartPm1Bus {
public:
    bool begin() {
        if (begun_) return true;
        mutex_ = xSemaphoreCreateMutex();
        if (!mutex_) return false;

        // UART0 is the board's J4 Serial path. Only RX is consumed by this
        // source; it has no ESP-to-producer commands or acknowledgements.
        Serial.begin(kUartBaud, SERIAL_8N1, kUartRxPin, kNoTxPin);
        begun_ = true;
        return true;
    }

    SensorSample read(uint8_t channel) {
        SensorSample unavailable;
        if (!begun_ || !mutex_ || channel >= 3) {
            unavailable.state = SensorSampleState::Invalid;
            return unavailable;
        }

        xSemaphoreTake(mutex_, portMAX_DELAY);
        const uint8_t bit = static_cast<uint8_t>(1U << channel);
        // Re-reading a channel marks a new acquisition cycle even if a caller
        // did not request all three channels in the previous cycle.
        if (servedMask_ == 0 || (servedMask_ & bit) != 0) refreshSnapshot();
        const SensorSample result = snapshot_[channel];
        servedMask_ |= bit;
        if (servedMask_ == sensors::pm1::kSupportedChannelMask) servedMask_ = 0;
        xSemaphoreGive(mutex_);
        return result;
    }

    sensors::pm1::Diagnostics diagnostics() {
        if (!mutex_) return {};
        xSemaphoreTake(mutex_, portMAX_DELAY);
        const sensors::pm1::Diagnostics result = receiver_.diagnostics();
        xSemaphoreGive(mutex_);
        return result;
    }

    uint32_t lastValidAgeMs() {
        if (!mutex_) return 0;
        xSemaphoreTake(mutex_, portMAX_DELAY);
        const uint32_t result = receiver_.lastValidAgeMs(millis());
        xSemaphoreGive(mutex_);
        return result;
    }

private:
    void refreshSnapshot() {
        const uint32_t receiveMs = millis();
        uint8_t bytes[64];
        while (Serial.available() > 0) {
            size_t count = 0;
            while (count < sizeof(bytes) && Serial.available() > 0) {
                const int value = Serial.read();
                if (value < 0) break;
                bytes[count++] = static_cast<uint8_t>(value);
            }
            if (count == 0) break;
            receiver_.feed(bytes, count, receiveMs);
        }
        const uint32_t snapshotMs = millis();
        for (uint8_t channel = 0; channel < 3; ++channel) {
            snapshot_[channel] = receiver_.sample(channel, snapshotMs);
        }
        servedMask_ = 0;
    }

    bool begun_ = false;
    SemaphoreHandle_t mutex_ = nullptr;
    sensors::pm1::Receiver receiver_;
    SensorSample snapshot_[3];
    uint8_t servedMask_ = 0;
};

UartPm1Bus bus;

} // namespace

bool UartPm1SensorSource::init() {
    return channel_ < 3 && bus.begin();
}

SensorSample UartPm1SensorSource::read() {
    return bus.read(channel_);
}

namespace sensors {

pm1::Diagnostics getUartDiagnostics() {
    return bus.diagnostics();
}

uint32_t getUartLastValidAgeMs() {
    return bus.lastValidAgeMs();
}

} // namespace sensors
