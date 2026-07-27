#pragma once

#include <cstddef>
#include <cstdint>

namespace historical_storage {

constexpr uint8_t kSensorCount = 3;
constexpr uint8_t kFlushIntervalMinutes = 5;
constexpr uint16_t kRecordsPerSegment = 240;
constexpr uint16_t kMaxHistoryFiles = 200;
constexpr uint32_t kMaterialGapMs = 60000;
constexpr uint32_t kInferenceBoundaryMs = 300000;

enum class Dataset : uint8_t { Real = 0, Demo = 1 };

enum Component : uint8_t {
    BATTERY_CHARGING = 0,
    BATTERY_USAGE,
    PANEL_IN,
    PANEL_USAGE,
    PANEL_SURPLUS,
    COMPONENT_COUNT,
};

enum QualityFlags : uint8_t {
    QUALITY_NONE = 0,
    QUALITY_REJECTED = 1 << 0,
    QUALITY_STALE_OR_MISSING = 1 << 1,
};

// History V1 ingress contract. The sensor/calculation layer keeps diagnostic
// observations; storage receives only eligible engineering-unit powers plus
// explicit masks. An unavailable channel/component is never represented by a
// zero value.
struct SampleFrame {
    float channelPowerW[kSensorCount]{};
    float componentPowerW[COMPONENT_COUNT]{};
    uint32_t timestampMs = 0;
    uint8_t configuredChannelMask = 0;
    uint8_t eligibleChannelMask = 0;
    uint8_t eligibleComponentMask = 0;
    uint8_t qualityFlags = QUALITY_NONE;
};

// Filename metadata supplies session and monotonic minute. Coverage makes a
// measured zero distinguishable from a missing/invalid observation.
struct __attribute__((packed)) MinuteEnergyRecordV1 {
    float channelEnergyWh[kSensorCount];
    float componentEnergyWh[COMPONENT_COUNT];
    uint16_t channelCoverageMs[kSensorCount];
    uint16_t componentCoverageMs[COMPONENT_COUNT];
    uint8_t configuredChannelMask;
    uint8_t qualityFlags;
    uint16_t reserved16;
    uint32_t reserved32;
};
static_assert(sizeof(MinuteEnergyRecordV1) == 56, "history V1 rows must remain 56 bytes");
using MinuteEnergyRecord = MinuteEnergyRecordV1;

enum TimeFlags : uint8_t {
    TIME_NONE = 0,
    TIME_ANCHORED = 1 << 0,
    TIME_INFERRED = 1 << 1,
    TIME_INCOMPLETE = 1 << 2,
};

enum class TimelineBasis : uint8_t {
    WallClock = 0,
    CurrentSessionMonotonic = 1,
};

struct PowerBucket {
    uint16_t durationMinutes;
    uint8_t timeFlags;
    uint8_t qualityFlags;
    int64_t startTimeMs;
    uint32_t coveredMs;
    float energyWh[kSensorCount];
    float componentEnergyWh[COMPONENT_COUNT];
    float componentAveragePowerW[COMPONENT_COUNT];
    uint32_t channelCoverageMs[kSensorCount];
    uint32_t componentCoverageMs[COMPONENT_COUNT];
    uint8_t configuredChannelMask;
    uint8_t reserved[3];
};

enum class CalendarRange : uint8_t {
    Today,
    Yesterday,
    Last2Days,
    LastWeek,
    LastTwoWeeks,
    All,
};

struct QueryStatus {
    int64_t startTimeMs;
    int64_t endTimeMs;
    TimelineBasis timelineBasis;
    uint32_t coveredMinutes;
    uint32_t missingMinutes;
    uint32_t inferredMinutes;
    uint32_t recordsRead;
    uint16_t filesRead;
    bool incomplete;
    bool hasInferredTime;
};

enum class FileState : uint8_t { Closed, Interrupted, Active };

struct HistoryFileInfo {
    char name[52];
    uint32_t sessionId;
    uint32_t firstMinute;
    uint16_t committedRecords;
    uint8_t bufferedRecords;
    FileState state;
    uint32_t bytes;
    int64_t startUnixMs;
    int64_t endUnixMs;
    uint8_t timeFlags;
    bool fixture;
};

struct StorageStats {
    uint16_t fileCount;
    uint16_t fixtureFileCount;
    uint32_t committedRecords;
    uint8_t bufferedRecords;
    uint32_t committedBytes;
    uint32_t bufferedBytes;
    uint16_t maxFiles;
};

bool init();
Dataset activeDataset();

// Dataset is explicit only at this internal storage boundary so provenance,
// not a UI/API parameter, determines the write tenant.
void addSampleFrame(Dataset dataset, const SampleFrame& frame);
void tick();

size_t getPowerBuckets(PowerBucket* out, size_t maxBuckets,
                       uint32_t lookbackMinutes, uint16_t bucketMinutes,
                       uint32_t endOffsetMinutes = 0, bool includePartial = true,
                       QueryStatus* status = nullptr);
size_t getCalendarPowerBuckets(PowerBucket* out, size_t maxBuckets,
                               CalendarRange range, uint16_t bucketMinutes,
                               QueryStatus* status = nullptr);
size_t getSinceBootPowerBuckets(PowerBucket* out, size_t maxBuckets,
                                uint16_t bucketMinutes = 0,
                                QueryStatus* status = nullptr);

// Internal dataset-aware variants support the on-device view filter and
// deterministic tests. External HTTP handlers continue to call the wrappers
// above, which follow activeDataset().
size_t getPowerBucketsForDataset(Dataset dataset, PowerBucket* out, size_t maxBuckets,
                                 uint32_t lookbackMinutes, uint16_t bucketMinutes,
                                 uint32_t endOffsetMinutes = 0, bool includePartial = true,
                                 QueryStatus* status = nullptr);
size_t getCalendarPowerBucketsForDataset(Dataset dataset, PowerBucket* out, size_t maxBuckets,
                                         CalendarRange range, uint16_t bucketMinutes,
                                         QueryStatus* status = nullptr);
size_t getSinceBootPowerBucketsForDataset(Dataset dataset, PowerBucket* out, size_t maxBuckets,
                                          uint16_t bucketMinutes = 0,
                                          QueryStatus* status = nullptr);

// Internal shared-model query for feature-specific wall-clock aggregation
// (for example energy cycles). Browser endpoints should expose their feature
// model rather than accepting arbitrary timestamps from clients.
size_t getTimePowerBuckets(PowerBucket* out, size_t maxBuckets,
                           int64_t startUnixMs, int64_t endUnixMs,
                           uint16_t bucketMinutes, QueryStatus* status = nullptr);
size_t getTimePowerBucketsForDataset(Dataset dataset, PowerBucket* out, size_t maxBuckets,
                                     int64_t startUnixMs, int64_t endUnixMs,
                                     uint16_t bucketMinutes, QueryStatus* status = nullptr);
size_t listFiles(HistoryFileInfo* out, size_t limit, size_t offset = 0,
                 size_t* total = nullptr, StorageStats* stats = nullptr);
size_t listFilesForDataset(Dataset dataset, HistoryFileInfo* out, size_t limit,
                           size_t offset = 0, size_t* total = nullptr,
                           StorageStats* stats = nullptr);
void getStorageStats(StorageStats& out);
void getStorageStatsForDataset(Dataset dataset, StorageStats& out);
bool clearAll();
bool clearDataset(Dataset dataset);
bool clearAllDatasets();
size_t recordCount();
size_t recordCountForDataset(Dataset dataset);

} // namespace historical_storage
