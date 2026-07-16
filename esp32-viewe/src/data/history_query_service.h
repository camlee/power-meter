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

bool init();
uint32_t requestUsage(const UsageRequest& request);
uint32_t requestCycles();
uint32_t requestFiles(size_t limit = kMaxListedFiles);
uint32_t requestFilesForDataset(historical_storage::Dataset dataset,
                                size_t limit = kMaxListedFiles);

bool takeUsage(uint32_t jobId, historical_storage::PowerBucket* out, size_t maxBuckets,
               size_t& count, historical_storage::QueryStatus& status, Timing* timing = nullptr);
bool takeCycles(uint32_t jobId, energy_cycle::Summary* out, size_t maxSummaries,
                size_t& count, Timing* timing = nullptr);
bool takeFiles(uint32_t jobId, historical_storage::HistoryFileInfo* out, size_t maxFiles,
               size_t& count, size_t& total, historical_storage::StorageStats& stats,
               Timing* timing = nullptr);

bool busy();
void getTiming(Timing& out);

} // namespace history_query_service
