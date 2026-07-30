#pragma once

#include <cstddef>
#include <cstdint>

namespace demo_scenarios {

constexpr uint32_t kScenarioDurationMs = 60000;
constexpr uint8_t kChannelCount = 3;

struct ChannelPoint {
    float voltage;
    float power;
};

struct Scenario {
    const char* name;
    const char* description;
    ChannelPoint channels[kChannelCount];
    float expectedBalanceW;
};

// These points describe the present three-channel contract used by Demo:
// channel 0 is incoming Solar, channel 1 is outgoing Load, and channel 2 is
// signed Battery power (positive charge, negative discharge). Phase (a)
// intentionally does not repair or reinterpret the downstream Usage logic.
constexpr Scenario kScenarios[] = {
    {
        "day-charge",
        "Solar supplies the load and charges the battery",
        {{20.0f, 40.0f}, {13.2f, 14.0f}, {13.2f, 26.0f}},
        0.0f,
    },
    {
        "solar-direct",
        "Solar supplies the load while the battery is idle",
        {{19.5f, 18.0f}, {13.0f, 18.0f}, {13.0f, 0.0f}},
        0.0f,
    },
    {
        "night-discharge",
        "The battery supplies the load with no solar production",
        {{0.0f, 0.0f}, {12.6f, 16.0f}, {12.6f, -16.0f}},
        0.0f,
    },
    {
        "balance-mismatch",
        "Installed-device-like readings with an intentional -8 W difference",
        {{20.0f, 20.0f}, {13.0f, 6.0f}, {13.0f, 22.0f}},
        -8.0f,
    },
    {
        "discharge-conflict",
        "Battery discharge exceeds Load with an intentional +8 W difference",
        {{19.0f, 6.0f}, {12.4f, 20.0f}, {12.4f, -22.0f}},
        8.0f,
    },
};

constexpr size_t kScenarioCount = sizeof(kScenarios) / sizeof(kScenarios[0]);

constexpr size_t scenarioIndex(uint32_t elapsedMs) {
    return (elapsedMs / kScenarioDurationMs) % kScenarioCount;
}

constexpr const Scenario& scenario(uint32_t elapsedMs) {
    return kScenarios[scenarioIndex(elapsedMs)];
}

} // namespace demo_scenarios
