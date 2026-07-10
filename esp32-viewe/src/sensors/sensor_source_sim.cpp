#include "sensor_source_sim.h"
#include <cmath>
#include <Arduino.h>

SimulatedSensorSource::SimulatedSensorSource(float voltageBaseline, float currentBaseline, uint32_t seedOffset)
    : voltageBaseline_(voltageBaseline), currentBaseline_(currentBaseline), seedOffset_(seedOffset) {}

bool SimulatedSensorSource::init() {
    startMs_ = millis();
    return true;
}

SensorSample SimulatedSensorSource::read() {
    float t = (millis() - startMs_) / 1000.0f;

    // Slow drift, phase-shifted per instance via seedOffset_ so the 3
    // simulated sensors don't look identical on the combined chart.
    float phase = (float)seedOffset_;
    float vDrift = 0.05f * voltageBaseline_ * sinf(t * 0.03f + phase);
    float iDrift = 0.15f * currentBaseline_ * sinf(t * 0.07f + phase * 1.7f);

    float vNoise = ((float)random(-1000, 1001) / 1000.0f) * 0.02f * voltageBaseline_;
    float iNoise = ((float)random(-1000, 1001) / 1000.0f) * 0.03f * currentBaseline_;

    SensorSample s;
    s.voltage = voltageBaseline_ + vDrift + vNoise;
    s.current = currentBaseline_ + iDrift + iNoise;
    if (s.current < 0) s.current = 0; // current shouldn't go negative in this sim
    return s;
}
