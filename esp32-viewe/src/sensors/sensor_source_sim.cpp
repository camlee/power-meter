#include "sensor_source_sim.h"
#include <cmath>
#include <Arduino.h>

SimulatedSensorSource::SimulatedSensorSource(float voltageBaseline, float currentBaseline, uint32_t seedOffset,
                                             float minimumDuty, float maximumDuty)
    : voltageBaseline_(voltageBaseline), currentBaseline_(currentBaseline), seedOffset_(seedOffset),
      minimumDuty_(minimumDuty), maximumDuty_(maximumDuty) {}

bool SimulatedSensorSource::init() {
    startMs_ = millis();
    lastDutyChangeMs_ = startMs_;
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
    s.state = SensorSampleState::Observed;
    s.configured = true;
    s.voltage = voltageBaseline_ + vDrift + vNoise;
    s.current = currentBaseline_ + iDrift + iNoise;
    if (s.current < 0) s.current = 0; // current shouldn't go negative in this sim

    // Model a 20 ms PWM period, far below the 500 ms acquisition cadence.
    // Each returned current is therefore the average of many on/off pulses,
    // not an instantaneous high/low sample that would draw square waves.
    if (maximumDuty_ < 1.0f) {
        const uint32_t now = millis();
        if (currentDuty_ >= 1.0f || now - lastDutyChangeMs_ >= 1000) {
            const long minimumPermille = lroundf(minimumDuty_ * 1000.0f);
            const long maximumPermille = lroundf(maximumDuty_ * 1000.0f);
            currentDuty_ = random(minimumPermille, maximumPermille + 1) / 1000.0f;
            lastDutyChangeMs_ = now;
        }
        s.current *= currentDuty_;
        s.hasDutyCycle = true;
        s.dutyCycle = currentDuty_;
    }
    return s;
}
