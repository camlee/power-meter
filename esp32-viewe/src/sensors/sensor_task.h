#pragma once
#include <cstddef>
#include <cstdint>

namespace sensor_task {

struct Reading {
    float value;
    uint32_t timestamp_ms;
};

constexpr size_t kHistorySize = 120; // in-RAM ring buffer depth; tune later

// Starts the background FreeRTOS task. Call once from setup(), after Serial.begin().
void start();

// Thread-safe. Copies up to maxCount most recent readings (oldest first) into out[].
// Returns how many were actually copied.
size_t getRecent(Reading* out, size_t maxCount);

// Thread-safe. Returns false if no reading has been taken yet.
bool getLatest(Reading& out);

} // namespace sensor_task
