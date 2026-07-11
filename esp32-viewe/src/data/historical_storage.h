#pragma once
#include <cstddef>
#include <cstdint>

// Long-term storage: one aggregated record per minute per sensor, written
// to internal flash via LittleFS. Deliberately decoupled from sensors.h --
// it doesn't call sensors::getRecent() itself. Instead the app feeds it
// samples via addSample(), which keeps this module reusable if the sample
// source ever changes, and makes it easy to unit-test with fake data.
//
// The UI and a future API should derive chart power from energy, rather than
// averaging already-averaged power samples.  PowerBucket is that query result:
// each value is sum(energyWh) / the fixed bucket duration.
namespace historical_storage {

constexpr uint8_t kSensorCount = 3;

enum Component : uint8_t {
    BATTERY_CHARGING = 0,
    BATTERY_USAGE,
    PANEL_IN,
    PANEL_USAGE,
    PANEL_SURPLUS,
    COMPONENT_COUNT,
};

// Packed so the on-disk layout is stable and doesn't depend on compiler
// padding choices -- this struct is written to flash byte-for-byte.
struct __attribute__((packed)) MinuteRecord {
    uint32_t uptime_m;              // Minutes since boot (always valid)
    uint32_t epoch_s;               // 0 if NTP is not yet synced, valid epoch otherwise
    float energyWh[kSensorCount];
    float componentEnergyWh[COMPONENT_COUNT];
};

struct PowerBucket {
    uint32_t startUptime_m;         // First minute in this fixed bucket
    uint16_t durationMinutes;       // May be shorter for the in-progress bucket
    float energyWh[kSensorCount];
    float componentEnergyWh[COMPONENT_COUNT];
    float componentAveragePowerW[COMPONENT_COUNT];
};

// Mounts LittleFS and opens/creates the ring buffer file. Call once from
// setup(), any time after LittleFS.begin() would normally be called
// elsewhere in the project (this function calls it internally if needed).
// Returns false if the filesystem couldn't be mounted.
bool init();

// Feeds one coherent measurement frame into the in-progress minute record.
// The battery/panel split is calculated at this cadence before any minute or
// chart aggregation can hide alternating charge/discharge periods.
void addSampleFrame(float inPowerW, float outPowerW, float auxPowerW,
                    float availableInPowerW, uint32_t timestamp_ms);

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

// Returns complete, fixed-duration buckets in chronological order.  A bucket
// is returned only when all of its minute records are present and consecutive;
// this prevents a boot/outage from being displayed as fabricated zero energy.
// endOffsetMinutes selects an older relative window: for example, a 1440
// minute lookback with a 1440 minute offset is the prior 24-hour window.
size_t getPowerBuckets(PowerBucket* out, size_t maxBuckets,
                       uint32_t lookbackMinutes, uint16_t bucketMinutes,
                       uint32_t endOffsetMinutes = 0, bool includePartial = true);

// Total minute records currently stored.
size_t recordCount();

} // namespace historical_storage
