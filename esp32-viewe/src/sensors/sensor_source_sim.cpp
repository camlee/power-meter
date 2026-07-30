#include "sensor_source_sim.h"
#include "demo_scenarios.h"
#include <cmath>
#include <Arduino.h>

namespace {

size_t lastLoggedScenario = demo_scenarios::kScenarioCount;

constexpr float kTwoPi = 6.28318530718f;

} // namespace

SimulatedSensorSource::SimulatedSensorSource(uint8_t channel) : channel_(channel) {}

bool SimulatedSensorSource::init() {
    return channel_ < demo_scenarios::kChannelCount;
}

SensorSample SimulatedSensorSource::read() {
    // millis() and history storage share the device's boot-monotonic clock.
    // Using it directly makes transitions occur on exact minute boundaries,
    // matching the finest persisted Usage bucket.
    const uint32_t elapsedMs = millis();
    const size_t scenarioIndex = demo_scenarios::scenarioIndex(elapsedMs);
    const demo_scenarios::Scenario& scenario = demo_scenarios::kScenarios[scenarioIndex];
    const demo_scenarios::ChannelPoint& point = scenario.channels[channel_];

    if (channel_ == 0 && scenarioIndex != lastLoggedScenario) {
        lastLoggedScenario = scenarioIndex;
        Serial.printf(
            "demo: scenario=%s solar=%.1fW load=%.1fW battery=%+.1fW balance=%+.1fW\n",
            scenario.name, scenario.channels[0].power, scenario.channels[1].power,
            scenario.channels[2].power, scenario.expectedBalanceW);
    }

    // A small shared ripple makes live charts visibly active without random
    // noise or inter-channel energy disagreement. Replaying the same elapsed
    // time always produces the same observation.
    const float withinScenario =
        static_cast<float>(elapsedMs % demo_scenarios::kScenarioDurationMs);
    const float ripplePhase = kTwoPi * withinScenario / 15000.0f;
    const float voltageScale = 1.0f + 0.002f * sinf(ripplePhase);
    const float powerScale = 1.0f + 0.015f * sinf(ripplePhase);

    SensorSample s;
    s.state = SensorSampleState::Observed;
    s.configured = true;
    s.voltage = point.voltage * voltageScale;
    const float power = point.power * powerScale;
    s.current = std::fabs(s.voltage) > 0.001f ? power / s.voltage : 0.0f;
    return s;
}
