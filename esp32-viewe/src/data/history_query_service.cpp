#include "history_query_service.h"

#include <algorithm>
#include <cstring>

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "memory/heap_policy.h"

namespace history_query_service {
namespace {

enum class JobKind : uint8_t { None, Usage, Cycles, Files };

struct Job {
    JobKind kind = JobKind::None;
    uint32_t id = 0;
    UsageRequest usage{};
    size_t fileLimit = kMaxListedFiles;
    historical_storage::Dataset fileDataset = historical_storage::Dataset::Real;
};

SemaphoreHandle_t mutex = nullptr;
TaskHandle_t task = nullptr;
historical_storage::PowerBucket* usageBuffer = nullptr;
energy_cycle::Summary* cycleBuffer = nullptr;
historical_storage::HistoryFileInfo* filesBuffer = nullptr;
Job queued{};
uint32_t nextJobId = 0;
uint32_t requestedJobId = 0;
uint32_t completedJobId = 0;
JobKind completedKind = JobKind::None;
bool isBusy = false;
bool usageResultLeased = false;
uint32_t leasedUsageJobId = 0;
size_t usageCount = 0;
historical_storage::QueryStatus usageStatus{};
size_t cycleCount = 0;
size_t filesCount = 0;
size_t filesTotal = 0;
historical_storage::StorageStats filesStats{};
Timing lastTiming{};

class Lock {
public:
    Lock() : locked(mutex && xSemaphoreTake(mutex, portMAX_DELAY) == pdTRUE) {}
    ~Lock() { if (locked) xSemaphoreGive(mutex); }
    explicit operator bool() const { return locked; }
private:
    bool locked;
};

void runJob(const Job& job, Timing& timing) {
    const uint32_t started = millis();
    if (job.kind == JobKind::Usage) {
        historical_storage::QueryStatus status{};
        const size_t count = job.usage.calendar
            ? historical_storage::getCalendarPowerBuckets(
                  usageBuffer, kMaxUsageBuckets, job.usage.calendarRange,
                  job.usage.bucketMinutes, &status)
            : historical_storage::getPowerBuckets(
                  usageBuffer, kMaxUsageBuckets, job.usage.lookbackMinutes,
                  job.usage.bucketMinutes, 0, true, &status);
        Lock lock;
        if (!lock || requestedJobId != job.id) return;
        usageCount = count;
        usageStatus = status;
        completedKind = JobKind::Usage;
        completedJobId = job.id;
        timing.lastRecordsRead = status.recordsRead;
        timing.lastFilesRead = status.filesRead;
    } else if (job.kind == JobKind::Cycles) {
        const size_t count = energy_cycle::query(
            cycleBuffer, energy_cycle::kRecentCycleCount);
        Lock lock;
        if (!lock || requestedJobId != job.id) return;
        cycleCount = count;
        completedKind = JobKind::Cycles;
        completedJobId = job.id;
        timing.lastRecordsRead = 0;
        timing.lastFilesRead = 0;
    } else if (job.kind == JobKind::Files) {
        historical_storage::StorageStats stats{};
        size_t total = 0;
        const size_t count = historical_storage::listFilesForDataset(
            job.fileDataset, filesBuffer, std::min(job.fileLimit, kMaxListedFiles),
            0, &total, &stats);
        Lock lock;
        if (!lock || requestedJobId != job.id) return;
        filesCount = count;
        filesTotal = total;
        filesStats = stats;
        completedKind = JobKind::Files;
        completedJobId = job.id;
        timing.lastRecordsRead = 0;
        timing.lastFilesRead = static_cast<uint16_t>(count);
    }
    timing.lastDurationMs = millis() - started;
    timing.lastWasUsage = job.kind == JobKind::Usage || job.kind == JobKind::Cycles;
}

void taskFn(void*) {
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        for (;;) {
            Job job;
            {
                Lock lock;
                if (!lock || queued.kind == JobKind::None) {
                    if (lock) isBusy = false;
                    break;
                }
                job = queued;
                queued.kind = JobKind::None;
            }

            Timing timing{};
            runJob(job, timing);
            {
                Lock lock;
                if (lock && requestedJobId == job.id) {
                    timing.maxDurationMs = std::max(lastTiming.maxDurationMs, timing.lastDurationMs);
                    lastTiming = timing;
                    const char* kindName = job.kind == JobKind::Usage ? "usage" :
                                           job.kind == JobKind::Cycles ? "cycles" : "files";
                    Serial.printf("history_query: %s %lu ms, %u files, %lu rows\n",
                                  kindName,
                                  static_cast<unsigned long>(timing.lastDurationMs),
                                  timing.lastFilesRead,
                                  static_cast<unsigned long>(timing.lastRecordsRead));
                }
            }
        }
    }
}

uint32_t enqueue(Job job) {
    Lock lock;
    if (!lock || !task || usageResultLeased) return 0;
    job.id = ++nextJobId;
    if (!job.id) job.id = ++nextJobId;
    requestedJobId = job.id;
    completedJobId = 0;
    completedKind = JobKind::None;
    queued = job; // The newest range wins; an older result is discarded.
    isBusy = true;
    xTaskNotifyGive(task);
    return job.id;
}

} // namespace

bool init() {
    if (task) return true;
    if (!mutex) mutex = xSemaphoreCreateMutex();
    if (!mutex) return false;
    if (!usageBuffer) usageBuffer = static_cast<historical_storage::PowerBucket*>(
        heap_policy::callocPreferred(kMaxUsageBuckets, sizeof(historical_storage::PowerBucket)));
    if (!cycleBuffer) cycleBuffer = static_cast<energy_cycle::Summary*>(
        heap_policy::callocPreferred(energy_cycle::kRecentCycleCount, sizeof(energy_cycle::Summary)));
    if (!filesBuffer) filesBuffer = static_cast<historical_storage::HistoryFileInfo*>(
        heap_policy::callocPreferred(kMaxListedFiles, sizeof(historical_storage::HistoryFileInfo)));
    if (!usageBuffer || !cycleBuffer || !filesBuffer) {
        Serial.println("history_query: buffer allocation failed");
        return false;
    }
    // Keep filesystem aggregation off the Arduino/LVGL core and below the
    // sensor task. Bounded yields in the reader let the core-0 idle task run.
    return xTaskCreatePinnedToCore(taskFn, "history_query", 6144, nullptr,
                                   tskIDLE_PRIORITY, &task, 0) == pdPASS;
}

uint32_t requestUsage(const UsageRequest& request) {
    Job job{};
    job.kind = JobKind::Usage;
    job.usage = request;
    return enqueue(job);
}

uint32_t requestCycles() {
    Job job{};
    job.kind = JobKind::Cycles;
    return enqueue(job);
}

uint32_t requestFiles(size_t limit) {
    return requestFilesForDataset(historical_storage::activeDataset(), limit);
}

uint32_t requestFilesForDataset(historical_storage::Dataset dataset, size_t limit) {
    Job job{};
    job.kind = JobKind::Files;
    job.fileLimit = limit;
    job.fileDataset = dataset;
    return enqueue(job);
}

bool takeUsage(uint32_t jobId, historical_storage::PowerBucket* out, size_t maxBuckets,
               size_t& count, historical_storage::QueryStatus& status, Timing* timing) {
    Lock lock;
    if (!lock || !out || completedKind != JobKind::Usage || completedJobId != jobId) return false;
    count = std::min(usageCount, maxBuckets);
    memcpy(out, usageBuffer, count * sizeof(*out));
    status = usageStatus;
    if (timing) *timing = lastTiming;
    completedJobId = 0;
    completedKind = JobKind::None;
    return true;
}

bool acquireUsage(uint32_t jobId, UsageResultView& view, Timing* timing) {
    Lock lock;
    if (!lock || usageResultLeased || completedKind != JobKind::Usage ||
        completedJobId != jobId || !usageBuffer) return false;
    usageResultLeased = true;
    leasedUsageJobId = jobId;
    view.buckets = usageBuffer;
    view.count = usageCount;
    view.status = usageStatus;
    if (timing) *timing = lastTiming;
    return true;
}

void releaseUsage(uint32_t jobId) {
    Lock lock;
    if (!lock || !usageResultLeased || leasedUsageJobId != jobId) return;
    usageResultLeased = false;
    leasedUsageJobId = 0;
    completedJobId = 0;
    completedKind = JobKind::None;
}

bool takeCycles(uint32_t jobId, energy_cycle::Summary* out, size_t maxSummaries,
                size_t& count, Timing* timing) {
    Lock lock;
    if (!lock || !out || completedKind != JobKind::Cycles || completedJobId != jobId) return false;
    count = std::min(cycleCount, maxSummaries);
    memcpy(out, cycleBuffer, count * sizeof(*out));
    if (timing) *timing = lastTiming;
    completedJobId = 0;
    completedKind = JobKind::None;
    return true;
}

bool takeFiles(uint32_t jobId, historical_storage::HistoryFileInfo* out, size_t maxFiles,
               size_t& count, size_t& total, historical_storage::StorageStats& stats,
               Timing* timing) {
    Lock lock;
    if (!lock || !out || completedKind != JobKind::Files || completedJobId != jobId) return false;
    count = std::min(filesCount, maxFiles);
    memcpy(out, filesBuffer, count * sizeof(*out));
    total = filesTotal;
    stats = filesStats;
    if (timing) *timing = lastTiming;
    completedJobId = 0;
    completedKind = JobKind::None;
    return true;
}

bool busy() {
    Lock lock;
    return lock && isBusy;
}

void getTiming(Timing& out) {
    Lock lock;
    out = lock ? lastTiming : Timing{};
}

} // namespace history_query_service
