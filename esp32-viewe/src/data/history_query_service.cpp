#include "history_query_service.h"

#include <algorithm>
#include <cstring>

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "data/history_job_queue.h"
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
constexpr size_t kFinishedJobCapacity = 8;
history_job_queue::Queue<Job, kQueuedJobCapacity, kFinishedJobCapacity> jobs;
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

bool runJob(const Job& job, Timing& timing) {
    const uint32_t started = millis();
    if (job.kind == JobKind::Usage) {
        historical_storage::QueryStatus status{};
        size_t count = 0;
        switch (job.usage.kind) {
            case UsageQueryKind::Calendar:
                count = historical_storage::getCalendarPowerBuckets(
                    usageBuffer, kMaxUsageBuckets, job.usage.calendarRange,
                    job.usage.bucketMinutes, &status);
                break;
            case UsageQueryKind::SinceBoot:
                count = historical_storage::getSinceBootPowerBuckets(
                    usageBuffer, kMaxUsageBuckets, job.usage.bucketMinutes, &status);
                break;
            case UsageQueryKind::Rolling:
                count = historical_storage::getPowerBuckets(
                    usageBuffer, kMaxUsageBuckets, job.usage.lookbackMinutes,
                    job.usage.bucketMinutes, 0, true, &status, true);
                break;
        }
        timing.lastDurationMs = millis() - started;
        timing.lastRecordsRead = status.recordsRead;
        timing.lastFilesRead = status.filesRead;
        timing.lastWasUsage = true;
        Lock lock;
        if (!lock || jobs.state(job.id) != history_job_queue::State::Running) return false;
        usageCount = count;
        usageStatus = status;
    } else if (job.kind == JobKind::Cycles) {
        const size_t count = energy_cycle::query(
            cycleBuffer, energy_cycle::kRecentCycleCount);
        timing.lastDurationMs = millis() - started;
        timing.lastRecordsRead = 0;
        timing.lastFilesRead = 0;
        timing.lastWasUsage = true;
        Lock lock;
        if (!lock || jobs.state(job.id) != history_job_queue::State::Running) return false;
        cycleCount = count;
    } else if (job.kind == JobKind::Files) {
        historical_storage::StorageStats stats{};
        size_t total = 0;
        const size_t count = historical_storage::listFilesForDataset(
            job.fileDataset, filesBuffer, std::min(job.fileLimit, kMaxListedFiles),
            0, &total, &stats);
        timing.lastDurationMs = millis() - started;
        timing.lastRecordsRead = 0;
        timing.lastFilesRead = static_cast<uint16_t>(count);
        timing.lastWasUsage = false;
        Lock lock;
        if (!lock || jobs.state(job.id) != history_job_queue::State::Running) return false;
        filesCount = count;
        filesTotal = total;
        filesStats = stats;
    } else {
        return false;
    }

    Lock lock;
    if (!lock || !jobs.complete(job.id, millis())) return false;
    timing.maxDurationMs = std::max(lastTiming.maxDurationMs, timing.lastDurationMs);
    lastTiming = timing;
    return true;
}

void taskFn(void*) {
    for (;;) {
        Job job{};
        bool shouldRun = false;
        uint32_t expiredJobId = 0;
        TickType_t waitTicks = portMAX_DELAY;
        {
            Lock lock;
            if (lock) {
                const uint32_t now = millis();
                const Job* ready = jobs.ready();
                if (ready && !usageResultLeased &&
                    static_cast<uint32_t>(now - jobs.readySinceMs()) >= kResultTtlMs) {
                    expiredJobId = ready->id;
                    jobs.expireReady(now, kResultTtlMs);
                }
                shouldRun = jobs.beginNext(job);
                ready = jobs.ready();
                if (!shouldRun && ready && !usageResultLeased) {
                    const uint32_t elapsed = now - jobs.readySinceMs();
                    const uint32_t remaining = elapsed >= kResultTtlMs ? 1 : kResultTtlMs - elapsed;
                    waitTicks = std::max<TickType_t>(1, pdMS_TO_TICKS(remaining));
                }
            }
        }
        if (expiredJobId) {
            Serial.printf("history_query: result %lu expired before collection\n",
                          static_cast<unsigned long>(expiredJobId));
        }
        if (shouldRun) {
            Timing timing{};
            if (runJob(job, timing)) {
                const char* kindName = job.kind == JobKind::Usage ? "usage" :
                                       job.kind == JobKind::Cycles ? "cycles" : "files";
                Serial.printf("history_query: %s job %lu, %lu ms, %u files, %lu rows\n",
                              kindName,
                              static_cast<unsigned long>(job.id),
                              static_cast<unsigned long>(timing.lastDurationMs),
                              timing.lastFilesRead,
                              static_cast<unsigned long>(timing.lastRecordsRead));
            }
            continue;
        }
        ulTaskNotifyTake(pdTRUE, waitTicks);
    }
}

uint32_t enqueue(Job job) {
    uint32_t jobId = 0;
    {
        Lock lock;
        if (!lock || !task) return 0;
        jobId = jobs.enqueue(job);
    }
    if (jobId) xTaskNotifyGive(task);
    return jobId;
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

bool cancel(uint32_t jobId) {
    bool cancelled = false;
    {
        Lock lock;
        if (!lock || (usageResultLeased && leasedUsageJobId == jobId)) return false;
        cancelled = jobs.cancel(jobId);
    }
    if (cancelled) xTaskNotifyGive(task);
    return cancelled;
}

bool takeUsage(uint32_t jobId, historical_storage::PowerBucket* out, size_t maxBuckets,
               size_t& count, historical_storage::QueryStatus& status, Timing* timing) {
    bool consumed = false;
    {
        Lock lock;
        const Job* ready = lock ? jobs.ready() : nullptr;
        if (!ready || !out || ready->kind != JobKind::Usage || ready->id != jobId) return false;
        count = std::min(usageCount, maxBuckets);
        memcpy(out, usageBuffer, count * sizeof(*out));
        status = usageStatus;
        if (timing) *timing = lastTiming;
        consumed = jobs.consume(jobId);
    }
    if (consumed) xTaskNotifyGive(task);
    return consumed;
}

bool acquireUsage(uint32_t jobId, UsageResultView& view, Timing* timing) {
    Lock lock;
    const Job* ready = lock ? jobs.ready() : nullptr;
    if (!ready || usageResultLeased || ready->kind != JobKind::Usage ||
        ready->id != jobId || !usageBuffer) return false;
    usageResultLeased = true;
    leasedUsageJobId = jobId;
    view.buckets = usageBuffer;
    view.count = usageCount;
    view.status = usageStatus;
    if (timing) *timing = lastTiming;
    return true;
}

void releaseUsage(uint32_t jobId) {
    bool consumed = false;
    {
        Lock lock;
        if (!lock || !usageResultLeased || leasedUsageJobId != jobId) return;
        usageResultLeased = false;
        leasedUsageJobId = 0;
        consumed = jobs.consume(jobId);
    }
    if (consumed) xTaskNotifyGive(task);
}

bool takeCycles(uint32_t jobId, energy_cycle::Summary* out, size_t maxSummaries,
                size_t& count, Timing* timing) {
    bool consumed = false;
    {
        Lock lock;
        const Job* ready = lock ? jobs.ready() : nullptr;
        if (!ready || !out || ready->kind != JobKind::Cycles || ready->id != jobId) return false;
        count = std::min(cycleCount, maxSummaries);
        memcpy(out, cycleBuffer, count * sizeof(*out));
        if (timing) *timing = lastTiming;
        consumed = jobs.consume(jobId);
    }
    if (consumed) xTaskNotifyGive(task);
    return consumed;
}

bool takeFiles(uint32_t jobId, historical_storage::HistoryFileInfo* out, size_t maxFiles,
               size_t& count, size_t& total, historical_storage::StorageStats& stats,
               Timing* timing) {
    bool consumed = false;
    {
        Lock lock;
        const Job* ready = lock ? jobs.ready() : nullptr;
        if (!ready || !out || ready->kind != JobKind::Files || ready->id != jobId) return false;
        count = std::min(filesCount, maxFiles);
        memcpy(out, filesBuffer, count * sizeof(*out));
        total = filesTotal;
        stats = filesStats;
        if (timing) *timing = lastTiming;
        consumed = jobs.consume(jobId);
    }
    if (consumed) xTaskNotifyGive(task);
    return consumed;
}

JobState jobState(uint32_t jobId) {
    Lock lock;
    if (!lock) return JobState::Unknown;
    switch (jobs.state(jobId)) {
        case history_job_queue::State::Queued: return JobState::Queued;
        case history_job_queue::State::Running: return JobState::Running;
        case history_job_queue::State::Ready: return JobState::Ready;
        case history_job_queue::State::Gone: return JobState::Gone;
        default: return JobState::Unknown;
    }
}

const char* jobStateName(JobState state) {
    switch (state) {
        case JobState::Queued: return "queued";
        case JobState::Running: return "running";
        case JobState::Ready: return "ready";
        case JobState::Gone: return "gone";
        default: return "unknown";
    }
}

bool busy() {
    Lock lock;
    return lock && jobs.hasOutstanding();
}

void getTiming(Timing& out) {
    Lock lock;
    out = lock ? lastTiming : Timing{};
}

} // namespace history_query_service
