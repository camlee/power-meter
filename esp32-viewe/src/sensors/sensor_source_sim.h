#pragma once
#include "sensor_source.h"

// Fake sensor: slow sine-wave drift on both voltage and current, plus small
// jitter, so it behaves roughly like a real power rail under a slowly
// changing load. seedOffset gives each of the 3 instances a different phase
// so they don't all move in lockstep on screen.
class SimulatedSensorSource : public SensorSource {
public:
    SimulatedSensorSource(float voltageBaseline, float currentBaseline, uint32_t seedOffset,
                          float minimumDuty = 1.0f, float maximumDuty = 1.0f);

    bool init() override;
    SensorSample read() override;

private:
    float voltageBaseline_;
    float currentBaseline_;
    uint32_t seedOffset_;
    uint32_t startMs_ = 0;
    float minimumDuty_;
    float maximumDuty_;
    float currentDuty_ = 1.0f;
    uint32_t lastDutyChangeMs_ = 0;
};
