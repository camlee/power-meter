#pragma once

#include <cstddef>
#include <cstdint>

#include "historical_storage.h"
#include "energy_cycle.h"

// Executes potentially slow LittleFS history work away from LVGL. Results are
// copied into caller-owned buffers by the UI after a job completes.
namespace history_query_service {

constexpr size_t kMaxUsageBuckets = 336;
constexpr size_t kMaxListedFiles = 20;
constexpr size_t kQueuedJobCapacity = 4;
constexpr uint32_t kResultTtlMs = 10000;

enum class JobState : uint8_t {
    Unknown,
    Queued,
    Running,
    Ready,
    Gone,
};

struct UsageRequest {
    bool calendar;
    historical_storage::CalendarRange calendarRange;
    uint32_t lookbackMinutes;
    uint16_t bucketMinutes;
};

struct Timing {
    uint32_t lastDurationMs;
    uint32_t maxDurationMs;
    uint32_t lastRecordsRead;
    uint16_t lastFilesRead;
    bool lastWasUsage;
};

// Read-only lease over a completed usage result. While leased, new history
// jobs are rejected so the worker cannot overwrite the backing buffer. This
// lets HTTP serialize the result incrementally without allocating a duplicate
// bucket array or a second full response buffer.
struct UsageResultView {
    const historical_storage::PowerBucket* buckets = nullptr;
    size_t count = 0;
    historical_storage::QueryStatus status{};
};

bool init();
uint32_t requestUsage(const UsageRequest& request);
uint32_t requestCycles();
uint32_t requestFiles(size_t limit = kMaxListedFiles);
uint32_t requestFilesForDataset(historical_storage::Dataset dataset,
                                size_t limit = kMaxListedFiles);
bool cancel(uint32_t jobId);

bool takeUsage(uint32_t jobId, historical_storage::PowerBucket* out, size_t maxBuckets,
               size_t& count, historical_storage::QueryStatus& status, Timing* timing = nullptr);
bool acquireUsage(uint32_t jobId, UsageResultView& view, Timing* timing = nullptr);
void releaseUsage(uint32_t jobId);
bool takeCycles(uint32_t jobId, energy_cycle::Summary* out, size_t maxSummaries,
                size_t& count, Timing* timing = nullptr);
bool takeFiles(uint32_t jobId, historical_storage::HistoryFileInfo* out, size_t maxFiles,
               size_t& count, size_t& total, historical_storage::StorageStats& stats,
               Timing* timing = nullptr);

JobState jobState(uint32_t jobId);
const char* jobStateName(JobState state);
bool busy();
void getTiming(Timing& out);

} // namespace history_query_service
