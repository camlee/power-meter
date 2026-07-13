#include "history_query_service.h"

#include <algorithm>
#include <cstring>

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

namespace history_query_service {
namespace {

enum class JobKind : uint8_t { None, Usage, Files };

struct Job {
    JobKind kind = JobKind::None;
    uint32_t id = 0;
    UsageRequest usage{};
    size_t fileLimit = kMaxListedFiles;
};

SemaphoreHandle_t mutex = nullptr;
TaskHandle_t task = nullptr;
historical_storage::PowerBucket* usageBuffer = nullptr;
historical_storage::HistoryFileInfo* filesBuffer = nullptr;
Job queued{};
uint32_t nextJobId = 0;
uint32_t requestedJobId = 0;
uint32_t completedJobId = 0;
JobKind completedKind = JobKind::None;
bool isBusy = false;
size_t usageCount = 0;
historical_storage::QueryStatus usageStatus{};
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
    } else if (job.kind == JobKind::Files) {
        historical_storage::StorageStats stats{};
        size_t total = 0;
        const size_t count = historical_storage::listFiles(
            filesBuffer, std::min(job.fileLimit, kMaxListedFiles), 0, &total, &stats);
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
    timing.lastWasUsage = job.kind == JobKind::Usage;
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
                    Serial.printf("history_query: %s %lu ms, %u files, %lu rows\n",
                                  timing.lastWasUsage ? "usage" : "files",
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
    if (!lock || !task) return 0;
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
    if (!usageBuffer) usageBuffer = static_cast<historical_storage::PowerBucket*>(heap_caps_calloc(
        kMaxUsageBuckets, sizeof(historical_storage::PowerBucket), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!filesBuffer) filesBuffer = static_cast<historical_storage::HistoryFileInfo*>(heap_caps_calloc(
        kMaxListedFiles, sizeof(historical_storage::HistoryFileInfo), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!usageBuffer || !filesBuffer) {
        Serial.println("history_query: PSRAM allocation failed");
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

uint32_t requestFiles(size_t limit) {
    Job job{};
    job.kind = JobKind::Files;
    job.fileLimit = limit;
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
