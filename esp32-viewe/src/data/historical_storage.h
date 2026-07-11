#pragma once
#include <cstddef>
#include <cstdint>

// Long-term storage: one aggregated record per minute per sensor, written
// to internal flash via LittleFS. Deliberately decoupled from sensors.h --
// it doesn't call sensors::getRecent() itself. Instead the app feeds it
// samples via addSample(), which keeps this module reusable if the sample
// source ever changes, and makes it easy to unit-test with fake data.
//
// Storage backend note: this implementation uses a fixed-capacity ring
// buffer file on LittleFS (kMaxRecords, sized for internal flash). If/when
// you move to an SD card for longer retention, only this .cpp changes --
// the API below (addSample/tick/getRecent) stays the same, so
// screen_historical.cpp doesn't need to change at all.
namespace historical_storage {

constexpr uint8_t kSensorCount = 3;

// Packed so the on-disk layout is stable and doesn't depend on compiler
// padding choices -- this struct is written to flash byte-for-byte.
struct __attribute__((packed)) MinuteRecord {
    uint32_t uptime_m;              // Minutes since boot (always valid)
    uint32_t epoch_s;               // 0 if NTP is not yet synced, valid epoch otherwise
    float avgPowerW[kSensorCount];
    float energyWh[kSensorCount];
};

// Mounts LittleFS and opens/creates the ring buffer file. Call once from
// setup(), any time after LittleFS.begin() would normally be called
// elsewhere in the project (this function calls it internally if needed).
// Returns false if the filesystem couldn't be mounted.
bool init();

// Feeds one realtime power sample into the in-progress minute average.
// Call this at the same cadence sensors are sampled (e.g. once per
// sensors::kSampleIntervalMs from a hook in loop(), or later from a
// subscriber callback if sensors.h grows one). sensorIndex must be
// < kSensorCount.
void addSample(uint8_t sensorIndex, float powerW, uint32_t timestamp_ms);

// Call at least once per second from loop(). Checks whether a minute
// boundary has passed since the last call and, if so, finalizes the
// pending MinuteRecord and appends it to flash.
void tick();

// Copies up to maxCount most-recent minute records into `out`, oldest
// first. Returns the number actually copied.
size_t getRecent(MinuteRecord* out, size_t maxCount);

// Fetches a decimated time series for UI rendering.
// maxPoints: The size of the allocated 'out' buffer (e.g., chart width).
// lookbackMinutes: The time domain to query (e.g., 60 for 1h, 1440 for 1d, 10080 for 1w).
// Returns the actual number of populated records in 'out'.
size_t getTimeSeries(MinuteRecord* out, size_t maxPoints, uint32_t lookbackMinutes);

// Total records currently stored (<= kMaxRecords).
size_t recordCount();

} // namespace historical_storage
