#include "sensor_source_ads1115.h"

#if POWER_METER_HAS_ADS1115

#include <Arduino.h>
#include "device/i2c_bus.h"

namespace {

constexpr uint8_t kConversionRegister = 0x00;
constexpr uint8_t kConfigRegister = 0x01;
constexpr uint16_t kConfigBase = 0x8000 | // start single conversion
                                 0x0400 | // PGA ±2.048 V
                                 0x0100 | // single-shot mode
                                 0x00e0 | // 860 SPS
                                 0x0003;  // comparator disabled
constexpr uint32_t kConversionTimeoutUs = 10000;

sensors::Ads1115Diagnostics diagnostics{};

void noteFailure() {
    ++diagnostics.failedConversions;
    ++diagnostics.consecutiveFailures;
}

bool writeRegister(uint8_t reg, uint16_t value) {
    return i2c_bus::writeRegister16(POWER_METER_ADS1115_ADDRESS, reg, value);
}

bool readRegister(uint8_t reg, uint16_t& value) {
    return i2c_bus::readRegister16(POWER_METER_ADS1115_ADDRESS, reg, value);
}

bool beginDevice() {
    if (diagnostics.ready) return true;
    if (!i2c_bus::isReady()) return false;
    uint16_t config = 0;
    diagnostics.ready = readRegister(kConfigRegister, config);
    if (!diagnostics.ready) Serial.printf("ads1115: device not found at 0x%02x\n",
                                          POWER_METER_ADS1115_ADDRESS);
    else Serial.printf("ads1115: ready address=0x%02x config=0x%04x\n",
                       POWER_METER_ADS1115_ADDRESS, config);
    return diagnostics.ready;
}

bool readChannel(uint8_t channel, float& volts) {
    if (!diagnostics.ready || channel > 3) return false;
    const uint32_t startedUs = micros();
    const uint16_t mux = static_cast<uint16_t>((0x04U + channel) << 12);
    if (!writeRegister(kConfigRegister, kConfigBase | mux)) {
        noteFailure();
        return false;
    }
    uint16_t config = 0;
    do {
        delayMicroseconds(250);
        if (!readRegister(kConfigRegister, config)) {
            noteFailure();
            return false;
        }
    } while (!(config & 0x8000) && micros() - startedUs < kConversionTimeoutUs);
    if (!(config & 0x8000)) {
        noteFailure();
        return false;
    }
    uint16_t rawBits = 0;
    if (!readRegister(kConversionRegister, rawBits)) {
        noteFailure();
        return false;
    }
    const int16_t raw = static_cast<int16_t>(rawBits);
    volts = static_cast<float>(raw) * (2.048f / 32768.0f);
    diagnostics.lastConversionUs = micros() - startedUs;
    diagnostics.lastSuccessMs = millis();
    diagnostics.consecutiveFailures = 0;
    ++diagnostics.successfulConversions;
    return true;
}

} // namespace

Ads1115SensorSource::Ads1115SensorSource(uint8_t sensor, uint8_t voltageChannel,
                                         uint8_t currentChannel, bool configured)
    : sensor_(sensor), voltageChannel_(voltageChannel), currentChannel_(currentChannel),
      configured_(configured) {}

bool Ads1115SensorSource::init() {
    if (!configured_) return true;
    return beginDevice();
}

SensorSample Ads1115SensorSource::read() {
    SensorSample sample;
    if (!configured_) {
        sample.state = SensorSampleState::NotConfigured;
        sample.configured = false;
        return sample;
    }
    sample.configured = true;
    if (!readChannel(voltageChannel_, sample.voltage) ||
        !readChannel(currentChannel_, sample.current)) {
        sample.state = SensorSampleState::Invalid;
        return sample;
    }
    sample.state = SensorSampleState::Observed;
    return sample;
}

namespace sensors {
Ads1115Diagnostics getAds1115Diagnostics() {
    Ads1115Diagnostics result = diagnostics;
    result.lockTimeouts = i2c_bus::lockTimeoutCount();
    result.busErrors = i2c_bus::errorCount();
    return result;
}
} // namespace sensors

#else

namespace sensors {
Ads1115Diagnostics getAds1115Diagnostics() { return {}; }
} // namespace sensors

#endif
