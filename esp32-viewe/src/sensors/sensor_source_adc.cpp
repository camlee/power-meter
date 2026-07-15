#include "sensor_source_adc.h"
#include <Arduino.h>

Esp32AnalogSource::Esp32AnalogSource(uint8_t voltagePin, uint8_t currentPin)
    : voltagePin_(voltagePin), currentPin_(currentPin) {}

bool Esp32AnalogSource::init()
{
    pinMode(voltagePin_, INPUT);
    pinMode(currentPin_, INPUT);
    analogSetPinAttenuation(voltagePin_, ADC_11db);
    analogSetPinAttenuation(currentPin_, ADC_11db);
    return true;
}

SensorSample Esp32AnalogSource::read()
{
    const float voltageInputV = analogReadMilliVolts(voltagePin_) / 1000.0f;
    const float currentInputV = analogReadMilliVolts(currentPin_) / 1000.0f;

    SensorSample sample;
    sample.state = SensorSampleState::Observed;
    sample.configured = true;
    sample.voltage = voltageInputV;
    sample.current = currentInputV;
    return sample;
}
