#pragma once

#include <cstddef>
#include <cstdint>

namespace energy_cycle {

// Both local and browser views consume the same bounded recent window.
constexpr size_t kRecentCycleCount = 7;
constexpr uint8_t kDefaultEndHour = 20; // 8:00 PM local time
constexpr float kChargeEfficiency = 0.80f;
constexpr uint8_t kMinimumAvailablePercent = 5;
constexpr uint8_t kCompleteCoveragePercent = 95;

struct Summary {
    int64_t startUnixMs = 0;
    int64_t endUnixMs = 0;
    uint32_t expectedMs = 0;
    uint32_t validCoverageMs = 0;
    float chargeWh = 0.0f; // Solar input after kChargeEfficiency.
    float useWh = 0.0f;
    float netWh = 0.0f;
    bool chargeAvailable = false;
    bool useAvailable = false;
    bool netAvailable = false;
    bool current = false;
    bool incomplete = false;
    uint8_t qualityFlags = 0;
};

// Loads the user-selected local cycle boundary from NVS. Safe to call twice.
void init();
uint8_t endHour();
bool setEndHour(uint8_t hour);

// Returns the bounded recent window as contiguous 24-hour cycles, oldest first.
// The final result is the current partial cycle. Storage work must run on the
// history worker.
size_t query(Summary* out, size_t maxSummaries);

} // namespace energy_cycle
