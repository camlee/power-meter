#include "historical_storage.h"

#include <Arduino.h>
#include <LittleFS.h>
#include <algorithm>
#include <cstring>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <esp_heap_caps.h>

#include "demo_history_profile.h"
#include "../sensors/sensor_mode.h"
#include "../time/time_service.h"

namespace historical_storage {
namespace {

constexpr char kHistoryDir[] = "/history";
constexpr char kV3Dir[] = "/history/v3";
constexpr uint64_t kMinuteUs = 60000000ULL;
constexpr int64_t kMinuteMs = 60000LL;
constexpr int64_t kDayMs = 86400000LL;
constexpr size_t kRamCapacity = kRecordsPerSegment;
constexpr size_t kCatalogCapacity = kMaxHistoryFiles + 1;
constexpr uint32_t kDemoSpanMinutes = 3 * 24 * 60;
constexpr uint32_t kDemoInferredSegmentFirstMinute = 1920;

// Ten four-hour segments give the UI enough realistic history to exercise
// every range while keeping the synthetic footprint small. Deliberate gaps
// also make the incomplete-data treatment visible during demoing.
constexpr uint32_t kDemoSegmentStarts[] = {
    0, 240, 720, 960, 1440, 1920, 2400, 3120, 3840, 4080,
};

struct CatalogEntry {
    char name[52];
    uint32_t sessionId;
    uint32_t firstMinute;
    uint16_t records;
    uint32_t bytes;
    bool closed;
    bool active;
};

struct LocatedRecord {
    MinuteEnergyRecord energy;
    uint32_t sessionId;
    uint32_t minute;
};

SemaphoreHandle_t mutex = nullptr;
bool ready = false;
MinuteEnergyRecord* ram = nullptr;
uint16_t ramCount = 0;
uint32_t ramFirstMinute = 0;
char activeName[52]{};
uint32_t activeFirstMinute = 0;
uint16_t activeCommitted = 0;
bool activeExists = false;

bool boundaryInitialized = false;
uint32_t nextBoundaryMinute = 0;
double energyAccumWh[kSensorCount]{};
double componentAccumWh[COMPONENT_COUNT]{};
uint32_t lastFrameMs = 0;
bool haveLastFrame = false;
// All public storage operations hold the recursive storage mutex, so one
// bounded scratch catalog avoids putting ~14 KB on the small Arduino task stacks.
CatalogEntry* catalogScratch = nullptr;
size_t catalogCount = 0;
bool catalogValid = false;

class Lock {
public:
    explicit Lock(TickType_t timeoutTicks = portMAX_DELAY)
        : locked(mutex && xSemaphoreTakeRecursive(mutex, timeoutTicks) == pdTRUE) {}
    ~Lock() { if (locked) xSemaphoreGiveRecursive(mutex); }
    explicit operator bool() const { return locked; }
private:
    bool locked;
};

void makeOpenName(char* out, size_t size, uint32_t session, uint32_t firstMinute) {
    snprintf(out, size, "h3-s%010lu-m%010lu.open",
             static_cast<unsigned long>(session), static_cast<unsigned long>(firstMinute));
}

void makeClosedName(char* out, size_t size, uint32_t session, uint32_t firstMinute, uint16_t count) {
    snprintf(out, size, "h3-s%010lu-m%010lu-n%04u.bin",
             static_cast<unsigned long>(session), static_cast<unsigned long>(firstMinute), count);
}

bool parseName(const char* name, CatalogEntry& entry) {
    unsigned long session = 0, minute = 0;
    unsigned count = 0;
    int consumed = 0;
    bool closed = sscanf(name, "h3-s%10lu-m%10lu-n%4u.bin%n", &session, &minute, &count, &consumed) == 3 &&
                  name[consumed] == '\0' && count <= kRecordsPerSegment;
    if (!closed) {
        consumed = 0;
        if (sscanf(name, "h3-s%10lu-m%10lu.open%n", &session, &minute, &consumed) != 2 ||
            name[consumed] != '\0') return false;
    }
    memset(&entry, 0, sizeof(entry));
    strncpy(entry.name, name, sizeof(entry.name) - 1);
    entry.sessionId = static_cast<uint32_t>(session);
    entry.firstMinute = static_cast<uint32_t>(minute);
    entry.records = closed ? static_cast<uint16_t>(count) : 0;
    entry.closed = closed;
    return true;
}

bool catalogLess(const CatalogEntry& a, const CatalogEntry& b) {
    if (a.sessionId != b.sessionId) return a.sessionId < b.sessionId;
    return a.firstMinute < b.firstMinute;
}

bool isDemoEntry(const CatalogEntry& entry) { return entry.sessionId == 0; }

size_t buildCatalog(CatalogEntry* out, size_t capacity) {
    if (out == catalogScratch && catalogValid) return catalogCount;
    size_t count = 0;
    File directory = LittleFS.open(kV3Dir);
    if (directory && directory.isDirectory()) {
        File file = directory.openNextFile();
        while (file) {
            CatalogEntry entry{};
            const char* full = file.name();
            const char* base = strrchr(full, '/');
            base = base ? base + 1 : full;
            if (!file.isDirectory() && parseName(base, entry) && count < capacity) {
                entry.bytes = file.size();
                const uint16_t completeRows = static_cast<uint16_t>(entry.bytes / sizeof(MinuteEnergyRecord));
                entry.records = entry.closed ? std::min(entry.records, completeRows) : completeRows;
                entry.active = activeExists && strcmp(entry.name, activeName) == 0;
                out[count++] = entry;
            }
            file = directory.openNextFile();
        }
        directory.close();
    }

    if (activeExists) {
        bool found = false;
        for (size_t i = 0; i < count; ++i) {
            if (strcmp(out[i].name, activeName) == 0) { out[i].active = true; found = true; break; }
        }
        if (!found && count < capacity) {
            CatalogEntry entry{};
            parseName(activeName, entry);
            entry.active = true;
            out[count++] = entry;
        }
    }
    std::sort(out, out + count, catalogLess);
    if (out == catalogScratch) {
        catalogCount = count;
        catalogValid = true;
    }
    return count;
}

int catalogIndex(const char* name) {
    if (!catalogValid) buildCatalog(catalogScratch, kCatalogCapacity);
    for (size_t i = 0; i < catalogCount; ++i) {
        if (strcmp(catalogScratch[i].name, name) == 0) return static_cast<int>(i);
    }
    return -1;
}

void fullPath(char* out, size_t size, const char* name) {
    snprintf(out, size, "%s/%s", kV3Dir, name);
}

bool enforceRetention() {
    CatalogEntry* catalog = catalogScratch;
    size_t count = buildCatalog(catalog, kCatalogCapacity);
    bool ok = true;
    while (count >= kMaxHistoryFiles) {
        size_t victim = 0;
        while (victim < count && catalog[victim].active) ++victim;
        if (victim == count) return false;
        char path[80];
        fullPath(path, sizeof(path), catalog[victim].name);
        if (!LittleFS.remove(path)) return false;
        memmove(catalog + victim, catalog + victim + 1, (count - victim - 1) * sizeof(CatalogEntry));
        --count;
    }
    catalogCount = count;
    catalogValid = true;
    return ok;
}

bool ensureActive() {
    if (activeExists) return true;
    if (!ramCount || !enforceRetention()) return false;
    activeFirstMinute = ramFirstMinute;
    makeOpenName(activeName, sizeof(activeName), time_service::currentSessionId(), activeFirstMinute);
    activeCommitted = 0;
    activeExists = true;
    if (!catalogValid) buildCatalog(catalogScratch, kCatalogCapacity);
    if (catalogCount >= kCatalogCapacity) {
        activeExists = false;
        activeName[0] = '\0';
        return false;
    }
    CatalogEntry entry{};
    parseName(activeName, entry);
    entry.active = true;
    catalogScratch[catalogCount++] = entry;
    std::sort(catalogScratch, catalogScratch + catalogCount, catalogLess);
    return true;
}

bool closeActive() {
    if (!activeExists || activeCommitted != kRecordsPerSegment || ramCount) return false;
    char oldPath[80], newPath[80], closedName[52];
    fullPath(oldPath, sizeof(oldPath), activeName);
    makeClosedName(closedName, sizeof(closedName), time_service::currentSessionId(),
                   activeFirstMinute, activeCommitted);
    fullPath(newPath, sizeof(newPath), closedName);
    if (!LittleFS.rename(oldPath, newPath)) return false;
    const int index = catalogIndex(activeName);
    if (index >= 0) {
        strncpy(catalogScratch[index].name, closedName, sizeof(catalogScratch[index].name) - 1);
        catalogScratch[index].closed = true;
        catalogScratch[index].active = false;
        catalogScratch[index].records = activeCommitted;
    }
    activeExists = false;
    activeName[0] = '\0';
    activeCommitted = 0;
    return true;
}

bool flushRam() {
    if (!ramCount) return true;
    if (!ensureActive()) return false;
    char path[80];
    fullPath(path, sizeof(path), activeName);
    File file = LittleFS.open(path, "a");
    if (!file) return false;
    const size_t bytes = ramCount * sizeof(MinuteEnergyRecord);
    const bool ok = file.write(reinterpret_cast<const uint8_t*>(ram), bytes) == bytes;
    file.flush();
    file.close();
    if (!ok) return false;
    activeCommitted += ramCount;
    const int index = catalogIndex(activeName);
    if (index >= 0) {
        catalogScratch[index].records = activeCommitted;
        catalogScratch[index].bytes += bytes;
    }
    ramCount = 0;
    if (activeCommitted == kRecordsPerSegment) return closeActive();
    return true;
}

void appendCompletedMinute(uint32_t minute) {
    if (ramCount == kRamCapacity) {
        Serial.println("historical_storage: RAM segment full; flash append is failing");
        return;
    }
    if (!ramCount) ramFirstMinute = minute;
    MinuteEnergyRecord& record = ram[ramCount++];
    for (uint8_t i = 0; i < kSensorCount; ++i) {
        record.energyWh[i] = static_cast<float>(energyAccumWh[i]);
        energyAccumWh[i] = 0;
    }
    for (uint8_t i = 0; i < COMPONENT_COUNT; ++i) {
        record.componentEnergyWh[i] = static_cast<float>(componentAccumWh[i]);
        componentAccumWh[i] = 0;
    }
    if (ramCount == 1 && !ensureActive()) {
        Serial.println("historical_storage: active segment metadata unavailable");
    }
    if (ramCount >= kFlushIntervalMinutes || activeCommitted + ramCount == kRecordsPerSegment) {
        if (!flushRam()) Serial.println("historical_storage: retaining unwritten rows in RAM");
    }
}

bool readRecord(const CatalogEntry& entry, uint16_t index, File& file, LocatedRecord& out) {
    if (index >= entry.records + (entry.active ? ramCount : 0)) return false;
    if (index < entry.records) {
        if (!file) {
            char path[80];
            fullPath(path, sizeof(path), entry.name);
            file = LittleFS.open(path, "r");
        }
        if (!file || !file.seek(static_cast<uint32_t>(index) * sizeof(MinuteEnergyRecord)) ||
            file.read(reinterpret_cast<uint8_t*>(&out.energy), sizeof(out.energy)) != sizeof(out.energy)) return false;
    } else {
        out.energy = ram[index - entry.records];
    }
    out.sessionId = entry.sessionId;
    out.minute = entry.firstMinute + index;
    return true;
}

uint64_t sequenceOf(const LocatedRecord& record) {
    return (static_cast<uint64_t>(record.sessionId) << 32) | record.minute;
}

void clearBucket(PowerBucket& bucket) { memset(&bucket, 0, sizeof(bucket)); }

void addEnergy(PowerBucket& bucket, const MinuteEnergyRecord& record, double fraction = 1.0) {
    for (uint8_t i = 0; i < kSensorCount; ++i) bucket.energyWh[i] += record.energyWh[i] * fraction;
    for (uint8_t i = 0; i < COMPONENT_COUNT; ++i) bucket.componentEnergyWh[i] += record.componentEnergyWh[i] * fraction;
}

void finishBucket(PowerBucket& bucket) {
    if (!bucket.durationMinutes) return;
    for (uint8_t i = 0; i < COMPONENT_COUNT; ++i)
        bucket.componentAveragePowerW[i] = bucket.componentEnergyWh[i] * 60.0f / bucket.durationMinutes;
    const uint32_t expected = static_cast<uint32_t>(bucket.durationMinutes) * 60000U;
    if (expected > bucket.coveredMs + kMaterialGapMs) bucket.timeFlags |= TIME_INCOMPLETE;
}

bool directStart(const CatalogEntry& entry, int64_t& startMs) {
    return entry.sessionId != 0 && time_service::resolveUnixTimeMs(
        entry.sessionId, static_cast<uint64_t>(entry.firstMinute) * kMinuteUs, startMs);
}

int64_t localMidnightUtc(int64_t unixMs, int16_t offsetMinutes);

bool sessionBounds(const CatalogEntry* catalog, size_t count, uint32_t session,
                   uint32_t& firstMinute, uint32_t& endMinute) {
    bool found = false;
    firstMinute = UINT32_MAX;
    endMinute = 0;
    for (size_t i = 0; i < count; ++i) if (catalog[i].sessionId == session) {
        found = true;
        firstMinute = std::min(firstMinute, catalog[i].firstMinute);
        endMinute = std::max(endMinute, catalog[i].firstMinute + catalog[i].records +
                                      static_cast<uint32_t>(catalog[i].active ? ramCount : 0));
    }
    return found;
}

bool resolveDemoEntry(const CatalogEntry* catalog, size_t count, const CatalogEntry& entry,
                      int64_t& startMs, uint8_t& flags) {
    if (sensor_mode::get() != sensor_mode::Mode::Demo || entry.sessionId != 0 ||
        !time_service::hasCurrentTime()) return false;
    uint32_t firstMinute = 0, endMinute = 0;
    if (!sessionBounds(catalog, count, 0, firstMinute, endMinute)) return false;
    int64_t nowMs = 0;
    if (!time_service::resolveUnixTimeMs(time_service::currentSessionId(),
            time_service::monotonicUs(), nowMs)) return false;
    // The synthetic source deliberately spans complete local days, including
    // today through its upcoming midnight. Queries still clip at "now", so it
    // never displays future energy but can exercise full-day axes without a
    // misleading leading partial-day gap.
    const int64_t demoEnd = localMidnightUtc(nowMs, time_service::utcOffsetMinutes()) + kDayMs;
    startMs = demoEnd - static_cast<int64_t>(endMinute - entry.firstMinute) * kMinuteMs;
    // Keep one historical demo day marked as an estimated placement. This
    // exercises the Usage disclosure without changing how real files resolve.
    flags = entry.firstMinute == kDemoInferredSegmentFirstMinute
        ? TIME_INFERRED : TIME_ANCHORED;
    return true;
}

bool resolveEntry(const CatalogEntry* catalog, size_t count, const CatalogEntry& entry,
                  int64_t& startMs, uint8_t& flags,
                  int64_t maxInferenceUncertaintyMs = 2LL * kInferenceBoundaryMs) {
    if (resolveDemoEntry(catalog, count, entry, startMs, flags)) return true;
    if (directStart(entry, startMs)) { flags = TIME_ANCHORED; return true; }
    if (entry.sessionId == 0) return false;
    uint32_t firstMinute, endMinute;
    if (!sessionBounds(catalog, count, entry.sessionId, firstMinute, endMinute)) return false;

    bool havePrevious = false, haveNext = false;
    int64_t previousEnd = 0, nextStart = 0;
    uint32_t previousSession = 0, nextSession = UINT32_MAX;
    for (size_t i = 0; i < count; ++i) {
        int64_t resolved = 0;
        if (catalog[i].sessionId < entry.sessionId && directStart(catalog[i], resolved)) {
            const uint32_t rows = catalog[i].records + (catalog[i].active ? ramCount : 0);
            const int64_t end = resolved + static_cast<int64_t>(rows) * kMinuteMs;
            if (!havePrevious || catalog[i].sessionId > previousSession ||
                (catalog[i].sessionId == previousSession && end > previousEnd)) {
                previousEnd = end;
                previousSession = catalog[i].sessionId;
                havePrevious = true;
            }
        } else if (catalog[i].sessionId > entry.sessionId && directStart(catalog[i], resolved)) {
            if (!haveNext || catalog[i].sessionId < nextSession ||
                (catalog[i].sessionId == nextSession && resolved < nextStart)) {
                nextStart = resolved;
                nextSession = catalog[i].sessionId;
                haveNext = true;
            }
        }
    }
    if (!havePrevious || !haveNext) return false;

    uint32_t blockMinutes = 0, minutesBefore = 0;
    uint16_t blockSessions = 0, sessionIndex = 0;
    uint32_t lastSession = UINT32_MAX;
    for (size_t i = 0; i < count; ++i) {
        const uint32_t session = catalog[i].sessionId;
        if (session <= previousSession || session >= nextSession || session == lastSession) continue;
        lastSession = session;
        int64_t anchored = 0;
        if (directStart(catalog[i], anchored)) continue;
        uint32_t blockFirst = 0, blockEnd = 0;
        if (!sessionBounds(catalog, count, session, blockFirst, blockEnd)) continue;
        if (session < entry.sessionId) minutesBefore += blockEnd - blockFirst;
        if (session == entry.sessionId) sessionIndex = blockSessions;
        blockMinutes += blockEnd - blockFirst;
        ++blockSessions;
    }
    if (!blockSessions) return false;
    const int64_t slack = nextStart - previousEnd - static_cast<int64_t>(blockMinutes) * kMinuteMs;
    if (slack < 0 || slack > maxInferenceUncertaintyMs) return false;

    // Use a stable estimate inside the defensible interval. Equal distribution
    // keeps session order and makes every reboot boundary explicit without
    // pretending we know which individual outage consumed the spare time.
    const int64_t boundaryGap = slack / (blockSessions + 1);
    const int64_t sessionStart = previousEnd + static_cast<int64_t>(minutesBefore) * kMinuteMs +
        boundaryGap * (sessionIndex + 1);
    startMs = sessionStart + static_cast<int64_t>(entry.firstMinute - firstMinute) * kMinuteMs;
    flags = TIME_INFERRED;
    return true;
}

int64_t localMidnightUtc(int64_t unixMs, int16_t offsetMinutes) {
    const int64_t offsetMs = static_cast<int64_t>(offsetMinutes) * kMinuteMs;
    return ((unixMs + offsetMs) / kDayMs) * kDayMs - offsetMs;
}

uint16_t automaticBucketMinutes(int64_t spanMs) {
    constexpr uint16_t kChoices[] = {2, 5, 10, 15, 30, 60, 120, 240, 360, 720, 1440, 2880, 4320, 10080};
    constexpr uint32_t kTargetBars = 48;
    const uint64_t spanMinutes = static_cast<uint64_t>(std::max<int64_t>(spanMs, kMinuteMs)) / kMinuteMs;
    const uint64_t target = (spanMinutes + kTargetBars - 1) / kTargetBars;
    for (uint16_t choice : kChoices) if (choice >= target) return choice;
    return kChoices[sizeof(kChoices) / sizeof(kChoices[0]) - 1];
}

void seedDemoHistory() {
    if (sensor_mode::get() != sensor_mode::Mode::Demo) return;
    CatalogEntry* existing = catalogScratch;
    const size_t existingCount = buildCatalog(existing, kCatalogCapacity);
    size_t demoCount = 0;
    bool expectedDemo = true;
    for (size_t i = 0; i < existingCount; ++i) {
        if (isDemoEntry(existing[i])) {
            if (demoCount >= sizeof(kDemoSegmentStarts) / sizeof(kDemoSegmentStarts[0]) ||
                existing[i].firstMinute != kDemoSegmentStarts[demoCount] ||
                existing[i].records != kRecordsPerSegment || !existing[i].closed) {
                expectedDemo = false;
            }
            ++demoCount;
        }
    }
    const size_t expectedCount = sizeof(kDemoSegmentStarts) / sizeof(kDemoSegmentStarts[0]);
    if (demoCount == expectedCount && expectedDemo) return;

    // Replace legacy/partial synthetic data as one set. Demo rows are marked
    // by session zero, so real measurements and their anchors are untouched.
    for (size_t i = 0; i < existingCount; ++i) {
        if (!isDemoEntry(existing[i])) continue;
        char path[80];
        fullPath(path, sizeof(path), existing[i].name);
        if (LittleFS.exists(path) && !LittleFS.remove(path)) {
            Serial.printf("historical_storage: could not replace demo segment %s\n", existing[i].name);
            return;
        }
    }
    catalogValid = false;
    Serial.printf("historical_storage: seeding %u demo segments across %lu days\n",
                  static_cast<unsigned>(expectedCount),
                  static_cast<unsigned long>(kDemoSpanMinutes / (24 * 60)));
    for (uint32_t first : kDemoSegmentStarts) {
        char name[52], path[80];
        makeClosedName(name, sizeof(name), 0, first, kRecordsPerSegment);
        fullPath(path, sizeof(path), name);
        File file = LittleFS.open(path, "w");
        if (!file) break;
        for (uint16_t i = 0; i < kRecordsPerSegment; ++i) {
            const auto& point = demo::kDayProfile[((first + i) / 15) % 96];
            const float charge = point.chargeW10 / 10.0f;
            const float use = point.useW10 / 10.0f;
            const float panel = point.panelW10 / 10.0f;
            MinuteEnergyRecord record{};
            record.energyWh[0] = (charge + panel) / 60.0f;
            record.energyWh[1] = (use + panel) / 60.0f;
            record.componentEnergyWh[BATTERY_CHARGING] = charge / 60.0f;
            record.componentEnergyWh[BATTERY_USAGE] = use / 60.0f;
            record.componentEnergyWh[PANEL_IN] = panel / 60.0f;
            record.componentEnergyWh[PANEL_USAGE] = panel / 60.0f;
            record.componentEnergyWh[PANEL_SURPLUS] = point.surplusW10 / 600.0f;
            file.write(reinterpret_cast<const uint8_t*>(&record), sizeof(record));
        }
        file.close();
    }
    catalogValid = false;
}

} // namespace

bool init() {
    if (!mutex) mutex = xSemaphoreCreateRecursiveMutex();
    Lock lock;
    if (!lock) return false;
    if (!LittleFS.begin(true)) { Serial.println("historical_storage: LittleFS mount failed"); return false; }
    if (!LittleFS.exists(kHistoryDir)) LittleFS.mkdir(kHistoryDir);
    if (!LittleFS.exists(kV3Dir)) LittleFS.mkdir(kV3Dir);
    if (!ram) ram = static_cast<MinuteEnergyRecord*>(heap_caps_calloc(
        kRamCapacity, sizeof(MinuteEnergyRecord), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!catalogScratch) catalogScratch = static_cast<CatalogEntry*>(heap_caps_calloc(
        kCatalogCapacity, sizeof(CatalogEntry), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!ram || !catalogScratch) {
        Serial.println("historical_storage: PSRAM allocation failed");
        return false;
    }
    time_service::init();
    ready = true;
    seedDemoHistory();
    CatalogEntry* catalog = catalogScratch;
    const size_t files = buildCatalog(catalog, kCatalogCapacity);
    Serial.printf("historical_storage: V3 ready, %u measurement files\n", static_cast<unsigned>(files));
    return true;
}

void addSampleFrame(float inPowerW, float outPowerW, float auxPowerW,
                    float availableInPowerW, uint32_t timestampMs) {
    // A long, background history read must never stall the Arduino loop (or
    // remote input). The next accepted frame integrates the elapsed interval,
    // which preserves a bounded approximation without blocking control work.
    Lock lock(0);
    if (!lock || !ready || !boundaryInitialized) return;
    if (haveLastFrame) {
        const uint32_t dtMs = timestampMs - lastFrameMs;
        // A stalled producer must not manufacture hours of energy on recovery.
        if (dtMs <= 10000) {
            const double hours = static_cast<double>(dtMs) / 3600000.0;
            const float in = std::max(inPowerW, 0.0f);
            const float out = std::max(outPowerW, 0.0f);
            const float aux = std::max(auxPowerW, 0.0f);
            const float netBattery = in - out;
            const float panelToLoad = std::min(in, out);
            energyAccumWh[0] += in * hours;
            energyAccumWh[1] += out * hours;
            energyAccumWh[2] += aux * hours;
            componentAccumWh[BATTERY_CHARGING] += std::max(netBattery, 0.0f) * hours;
            componentAccumWh[BATTERY_USAGE] += std::max(-netBattery, 0.0f) * hours;
            componentAccumWh[PANEL_IN] += panelToLoad * hours;
            componentAccumWh[PANEL_USAGE] += panelToLoad * hours;
            componentAccumWh[PANEL_SURPLUS] += std::max(availableInPowerW - in, 0.0f) * hours;
        }
    }
    lastFrameMs = timestampMs;
    haveLastFrame = true;
}

void tick() {
    // A missed tick while a query owns the storage mutex is harmless; the
    // next loop pass catches up without freezing network/control processing.
    Lock lock(0);
    if (!lock || !ready) return;
    const uint32_t minute = static_cast<uint32_t>(time_service::monotonicUs() / kMinuteUs);
    if (!boundaryInitialized) {
        nextBoundaryMinute = minute + 1;
        memset(energyAccumWh, 0, sizeof(energyAccumWh));
        memset(componentAccumWh, 0, sizeof(componentAccumWh));
        haveLastFrame = false;
        boundaryInitialized = true;
        return;
    }
    if (minute < nextBoundaryMinute) return;
    appendCompletedMinute(nextBoundaryMinute - 1);
    // If execution was suspended across several boundaries, preserve the
    // aggregate as one row and resume at the next complete minute.
    nextBoundaryMinute = minute + 1;
}

size_t queryTimeBuckets(PowerBucket* out, size_t maxBuckets, int64_t startMs,
                        int64_t endMs, int64_t nowMs, uint16_t bucketMinutes,
                        QueryStatus* status) {
    if (status) *status = {};
    if (!out || !maxBuckets || !bucketMinutes || endMs <= startMs) return 0;
    const int64_t bucketMs = static_cast<int64_t>(bucketMinutes) * kMinuteMs;
    size_t bucketCount = static_cast<size_t>((endMs - startMs + bucketMs - 1) / bucketMs);
    if (bucketCount > maxBuckets) {
        startMs = endMs - static_cast<int64_t>(maxBuckets) * bucketMs;
        bucketCount = maxBuckets;
    }
    for (size_t i = 0; i < bucketCount; ++i) {
        clearBucket(out[i]);
        out[i].startUnixMs = startMs + static_cast<int64_t>(i) * bucketMs;
        const int64_t finish = std::min(out[i].startUnixMs + bucketMs, endMs);
        out[i].durationMinutes = static_cast<uint16_t>(
            (finish - out[i].startUnixMs + kMinuteMs - 1) / kMinuteMs);
    }

    CatalogEntry* catalog = catalogScratch;
    const size_t files = buildCatalog(catalog, kCatalogCapacity);
    uint64_t coveredMs = 0, inferredMs = 0;
    const int64_t dataEndMs = std::min(endMs, nowMs);
    for (size_t f = 0; f < files; ++f) {
        int64_t fileStart = 0;
        uint8_t flags = TIME_NONE;
        if (!resolveEntry(catalog, files, catalog[f], fileStart, flags, bucketMs)) continue;
        const uint16_t rows = catalog[f].records + (catalog[f].active ? ramCount : 0);
        const int64_t fileEnd = fileStart + static_cast<int64_t>(rows) * kMinuteMs;
        if (fileEnd <= startMs || fileStart >= dataEndMs) continue;
        const uint16_t firstRow = fileStart < startMs
            ? static_cast<uint16_t>((startMs - fileStart) / kMinuteMs) : 0;
        const uint16_t lastRow = static_cast<uint16_t>(std::min<int64_t>(
            rows, (dataEndMs - fileStart + kMinuteMs - 1) / kMinuteMs));
        if (lastRow > firstRow && status) ++status->filesRead;
        File file;
        for (uint16_t r = firstRow; r < lastRow; ++r) {
            LocatedRecord record{};
            if (!readRecord(catalog[f], r, file, record)) continue;
            if (status) ++status->recordsRead;
            const int64_t recordStart = fileStart + static_cast<int64_t>(r) * kMinuteMs;
            const int64_t recordEnd = recordStart + kMinuteMs;
            const int64_t clippedStart = std::max(recordStart, startMs);
            const int64_t clippedEnd = std::min(recordEnd, dataEndMs);
            if (clippedEnd <= clippedStart) continue;
            const size_t firstBucket = static_cast<size_t>((clippedStart - startMs) / bucketMs);
            const size_t lastBucket = static_cast<size_t>((clippedEnd - 1 - startMs) / bucketMs);
            for (size_t b = firstBucket; b <= lastBucket && b < bucketCount; ++b) {
                const int64_t overlapStart = std::max(clippedStart, out[b].startUnixMs);
                const int64_t overlapEnd = std::min(clippedEnd, out[b].startUnixMs + bucketMs);
                const uint32_t overlap = static_cast<uint32_t>(
                    std::max<int64_t>(0, overlapEnd - overlapStart));
                if (!overlap) continue;
                addEnergy(out[b], record.energy, static_cast<double>(overlap) / kMinuteMs);
                out[b].coveredMs += overlap;
                out[b].timeFlags |= flags;
                if (!out[b].startSequence) out[b].startSequence = sequenceOf(record);
                coveredMs += overlap;
                if (flags & TIME_INFERRED) inferredMs += overlap;
            }
            // Usage aggregation runs in the low-priority history worker.
            // Let the core's idle/network tasks run between bounded batches;
            // a large "All" scan otherwise trips the task watchdog even
            // though LVGL itself is no longer blocked.
            if ((r & 0x3fU) == 0x3fU) vTaskDelay(pdMS_TO_TICKS(1));
        }
        if (file) file.close();
    }
    for (size_t i = 0; i < bucketCount; ++i) finishBucket(out[i]);

    const uint64_t elapsedSpan = dataEndMs > startMs ? dataEndMs - startMs : 0;
    const uint64_t missing = elapsedSpan > coveredMs ? elapsedSpan - coveredMs : 0;
    if (status) {
        status->startUnixMs = startMs;
        status->endUnixMs = endMs;
        status->coveredMinutes = coveredMs / kMinuteMs;
        status->missingMinutes = missing / kMinuteMs;
        status->inferredMinutes = inferredMs / kMinuteMs;
        status->incomplete = missing > kMaterialGapMs;
        status->hasInferredTime = inferredMs != 0;
    }
    return bucketCount;
}

size_t getPowerBuckets(PowerBucket* out, size_t maxBuckets,
                       uint32_t lookbackMinutes, uint16_t bucketMinutes,
                       uint32_t endOffsetMinutes, bool includePartial,
                       QueryStatus* status) {
    Lock lock;
    if (status) *status = {};
    if (!lock || !ready || !out || !maxBuckets || !lookbackMinutes || !bucketMinutes ||
        !time_service::hasCurrentTime()) return 0;
    int64_t nowMs = 0;
    if (!time_service::resolveUnixTimeMs(time_service::currentSessionId(),
            time_service::monotonicUs(), nowMs)) return 0;
    const int64_t endMs = nowMs - static_cast<int64_t>(endOffsetMinutes) * kMinuteMs;
    const int64_t startMs = endMs - static_cast<int64_t>(lookbackMinutes) * kMinuteMs;
    (void)includePartial;
    return queryTimeBuckets(out, maxBuckets, startMs, endMs, nowMs, bucketMinutes, status);
}

size_t getCalendarPowerBuckets(PowerBucket* out, size_t maxBuckets,
                               CalendarRange range, uint16_t bucketMinutes,
                               QueryStatus* status) {
    Lock lock;
    if (status) *status = {};
    if (!lock || !ready || !out || !maxBuckets ||
        (!bucketMinutes && range != CalendarRange::All) || !time_service::hasCurrentTime()) return 0;
    int64_t nowMs = 0;
    if (!time_service::resolveUnixTimeMs(time_service::currentSessionId(), time_service::monotonicUs(), nowMs)) return 0;
    const int64_t today = localMidnightUtc(nowMs, time_service::utcOffsetMinutes());
    int64_t startMs = today, endMs = nowMs;
    switch (range) {
        case CalendarRange::Today: endMs = today + kDayMs; break;
        case CalendarRange::Yesterday: startMs = today - kDayMs; endMs = today; break;
        case CalendarRange::Last2Days: startMs = today - kDayMs; endMs = today + kDayMs; break;
        case CalendarRange::LastWeek: startMs = today - 6 * kDayMs; break;
        case CalendarRange::LastTwoWeeks: startMs = today - 13 * kDayMs; break;
        case CalendarRange::All: {
            CatalogEntry* catalog = catalogScratch;
            const size_t files = buildCatalog(catalog, kCatalogCapacity);
            bool found = false;
            int64_t oldest = 0;
            const int64_t tolerance = static_cast<int64_t>(bucketMinutes) * kMinuteMs;
            for (size_t i = 0; i < files; ++i) {
                int64_t resolved = 0;
                uint8_t flags = TIME_NONE;
                if (resolveEntry(catalog, files, catalog[i], resolved, flags, tolerance) &&
                    (!found || resolved < oldest)) {
                    oldest = resolved;
                    found = true;
                }
            }
            if (!found) return 0;
            startMs = localMidnightUtc(oldest, time_service::utcOffsetMinutes());
            if (!bucketMinutes) bucketMinutes = automaticBucketMinutes(nowMs - startMs);
            break;
        }
    }
    return queryTimeBuckets(out, maxBuckets, startMs, endMs, nowMs, bucketMinutes, status);
}

size_t listFiles(HistoryFileInfo* out, size_t limit, size_t offset, size_t* total,
                 StorageStats* stats) {
    Lock lock;
    if (!lock || !ready) { if (total) *total = 0; return 0; }
    CatalogEntry* catalog = catalogScratch;
    const size_t count = buildCatalog(catalog, kCatalogCapacity);
    size_t visibleCount = 0;
    if (stats) {
        *stats = {};
        stats->maxFiles = kMaxHistoryFiles;
        for (size_t i = 0; i < count; ++i) {
            if (isDemoEntry(catalog[i])) continue;
            ++stats->fileCount;
            stats->committedRecords += catalog[i].records;
            stats->committedBytes += catalog[i].bytes;
        }
        stats->bufferedRecords = static_cast<uint8_t>(ramCount);
        stats->bufferedBytes = ramCount * sizeof(MinuteEnergyRecord);
    }
    for (size_t i = 0; i < count; ++i) if (!isDemoEntry(catalog[i])) ++visibleCount;
    if (total) *total = visibleCount;
    if (!out || !limit || offset >= visibleCount) return 0;
    size_t resultCount = 0;
    size_t skipped = 0;
    for (size_t index = count; index > 0 && resultCount < limit; --index) {
        const CatalogEntry& entry = catalog[index - 1];
        if (isDemoEntry(entry)) continue;
        if (skipped++ < offset) continue;
        HistoryFileInfo info{};
        strncpy(info.name, entry.name, sizeof(info.name) - 1);
        info.sessionId = entry.sessionId;
        info.firstMinute = entry.firstMinute;
        info.committedRecords = entry.records;
        info.bufferedRecords = entry.active ? static_cast<uint8_t>(ramCount) : 0;
        info.state = entry.active ? FileState::Active : (entry.closed ? FileState::Closed : FileState::Interrupted);
        info.bytes = entry.bytes;
        int64_t start = 0; uint8_t flags = TIME_NONE;
        if (resolveEntry(catalog, count, entry, start, flags)) {
            info.startUnixMs = start;
            info.endUnixMs = start + static_cast<int64_t>(entry.records + (entry.active ? ramCount : 0)) * kMinuteMs;
            info.timeFlags = flags;
        }
        out[resultCount++] = info;
    }
    return resultCount;
}

void getStorageStats(StorageStats& out) {
    listFiles(nullptr, 0, 0, nullptr, &out);
}

bool clearAll() {
    Lock lock;
    if (!lock || !ready) return false;
    bool ok = true;
    CatalogEntry* catalog = catalogScratch;
    const size_t count = buildCatalog(catalog, kCatalogCapacity);
    for (size_t i = 0; i < count; ++i) {
        if (isDemoEntry(catalog[i])) continue;
        char path[80];
        fullPath(path, sizeof(path), catalog[i].name);
        if (LittleFS.exists(path) && !LittleFS.remove(path)) ok = false;
    }
    ok &= time_service::clearHistoryAnchors();
    ramCount = 0; activeExists = false; activeName[0] = '\0'; activeCommitted = 0;
    catalogCount = 0; catalogValid = false;
    boundaryInitialized = false; haveLastFrame = false;
    memset(energyAccumWh, 0, sizeof(energyAccumWh));
    memset(componentAccumWh, 0, sizeof(componentAccumWh));
    return ok;
}

size_t recordCount() {
    StorageStats stats{};
    getStorageStats(stats);
    return stats.committedRecords + stats.bufferedRecords;
}

} // namespace historical_storage
