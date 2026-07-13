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

enum Component : uint8_t {
    BATTERY_CHARGING = 0,
    BATTERY_USAGE,
    PANEL_IN,
    PANEL_USAGE,
    PANEL_SURPLUS,
    COMPONENT_COUNT,
};

// Filename metadata supplies the session and monotonic minute. Keeping rows at
// exactly 32 bytes makes seeks cheap and five-minute flash writes exactly 160 B.
struct __attribute__((packed)) MinuteEnergyRecord {
    float energyWh[kSensorCount];
    float componentEnergyWh[COMPONENT_COUNT];
};
static_assert(sizeof(MinuteEnergyRecord) == 32, "history V3 rows must remain 32 bytes");

enum TimeFlags : uint8_t {
    TIME_NONE = 0,
    TIME_ANCHORED = 1 << 0,
    TIME_INFERRED = 1 << 1,
    TIME_INCOMPLETE = 1 << 2,
};

struct PowerBucket {
    uint32_t startUptime_m;
    uint16_t durationMinutes;
    uint8_t timeFlags;
    uint8_t reserved;
    uint64_t startSequence;
    int64_t startUnixMs;
    uint32_t coveredMs;
    float energyWh[kSensorCount];
    float componentEnergyWh[COMPONENT_COUNT];
    float componentAveragePowerW[COMPONENT_COUNT];
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
    int64_t startUnixMs;
    int64_t endUnixMs;
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
    uint8_t bufferedRecords; // Non-zero only for the active segment.
    FileState state;
    uint32_t bytes;
    int64_t startUnixMs;
    int64_t endUnixMs;
    uint8_t timeFlags;
};

struct StorageStats {
    uint16_t fileCount;
    uint32_t committedRecords;
    uint8_t bufferedRecords;
    uint32_t committedBytes;
    uint32_t bufferedBytes;
    uint16_t maxFiles;
};

bool init();
void addSampleFrame(float inPowerW, float outPowerW, float auxPowerW,
                    float availableInPowerW, uint32_t timestampMs);
void tick();

size_t getPowerBuckets(PowerBucket* out, size_t maxBuckets,
                       uint32_t lookbackMinutes, uint16_t bucketMinutes,
                       uint32_t endOffsetMinutes = 0, bool includePartial = true,
                       QueryStatus* status = nullptr);
size_t getCalendarPowerBuckets(PowerBucket* out, size_t maxBuckets,
                               CalendarRange range, uint16_t bucketMinutes,
                               QueryStatus* status = nullptr);

// Newest first. offset/limit make the same operation suitable for the device
// debug screen and the future browser app. No history row bodies are read.
size_t listFiles(HistoryFileInfo* out, size_t limit, size_t offset = 0,
                 size_t* total = nullptr, StorageStats* stats = nullptr);
void getStorageStats(StorageStats& out);
bool clearAll();
size_t recordCount();

} // namespace historical_storage
