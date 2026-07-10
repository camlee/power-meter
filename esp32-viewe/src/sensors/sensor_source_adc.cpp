#pragma once
#include "sensor_source.h"
#include <Arduino.h>

class ESP32AnalogSource : public SensorSource {
private:
    uint8_t v_pin;
    uint8_t i_pin;
    const float v_offset = 0.0f;
    const float v_gain = 0.027027f;
    const float i_offset = 1.667f;
    const float i_gain = 0.026667f;

public:
    ESP32AnalogSource(uint8_t v_pin, uint8_t i_pin) : v_pin(v_pin), i_pin(i_pin) {}

    bool init() override {
        pinMode(v_pin, INPUT);
        pinMode(i_pin, INPUT);
        analogSetPinAttenuation(v_pin, ADC_11db);
        analogSetPinAttenuation(i_pin, ADC_11db);
        return true;
    }

    SensorSample read() override {
        float v_mv = analogReadMilliVolts(v_pin) / 1000.0f;
        float i_mv = analogReadMilliVolts(i_pin) / 1000.0f;
        
        return {
            (v_mv - v_offset) / v_gain,
            (i_mv - i_offset) / i_gain
        };
    }
};