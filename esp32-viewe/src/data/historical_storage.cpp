#include "historical_storage.h"

#include <Arduino.h>
#include <LittleFS.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "demo_history_profile.h"
#include "../sensors/sensor_mode.h"
#include "../time/time_service.h"

namespace historical_storage {
namespace {

constexpr char kHistoryDir[] = "/history";
constexpr char kV1Dir[] = "/history/v1";
constexpr char kRealDir[] = "/history/v1/real";
constexpr char kDemoDir[] = "/history/v1/demo";
constexpr char kFixtureMarkerPath[] = "/history/v1/demo/.fixture-version";
constexpr char kFixtureAnchorPath[] = "/history/v1/demo/.fixture-anchor";
constexpr char kFixtureAnchorTempPath[] = "/history/v1/demo/.fixture-anchor.tmp";
constexpr uint32_t kFixtureVersion = 2;
constexpr uint32_t kFixtureAnchorMagic = 0x31414846; // FHA1
constexpr uint64_t kMinuteUs = 60000000ULL;
constexpr int64_t kMinuteMs = 60000LL;
constexpr int64_t kDayMs = 86400000LL;
constexpr int64_t kFixtureAgeMs = 2 * kDayMs;
constexpr uint32_t kMaxFrameIntervalMs = 10000;
constexpr uint32_t kBoundaryGraceMs = kMaxFrameIntervalMs + 1000;
constexpr size_t kRamCapacity = kRecordsPerSegment;
constexpr size_t kCatalogCapacity = kMaxHistoryFiles + 1;

constexpr size_t kDemoSegmentCount = demo::kFixtureSegmentCount;
constexpr uint32_t kDemoSpanMinutes = demo::kFixtureSpanMinutes;

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

struct __attribute__((packed)) FixtureAnchor {
    uint32_t magic;
    uint32_t version;
    int64_t minuteZeroUnixMs;
    int16_t utcOffsetMinutes;
    uint16_t reserved;
};
static_assert(sizeof(FixtureAnchor) == 20, "fixture anchor layout changed");

SemaphoreHandle_t mutex = nullptr;
bool ready = false;
MinuteEnergyRecord* ram = nullptr;
uint16_t ramCount = 0;
uint32_t ramFirstMinute = 0;
char activeName[52]{};
uint32_t activeFirstMinute = 0;
uint16_t activeCommitted = 0;
bool activeExists = false;
Dataset recordingDataset = Dataset::Real;
bool haveRecordingDataset = false;

bool boundaryInitialized = false;
uint32_t nextBoundaryMinute = 0;
uint32_t nextBoundaryTimestampMs = 0;
double channelEnergyAccumWh[kSensorCount]{};
double componentEnergyAccumWh[COMPONENT_COUNT]{};
uint32_t channelCoverageAccumMs[kSensorCount]{};
uint32_t componentCoverageAccumMs[COMPONENT_COUNT]{};
uint8_t minuteConfiguredMask = 0;
uint8_t minuteQualityFlags = QUALITY_NONE;
SampleFrame lastFrame{};
bool haveLastFrame = false;
bool runBreakPending = false;

CatalogEntry* catalogScratch = nullptr;
size_t catalogCount = 0;
bool catalogValid = false;
Dataset catalogDataset = Dataset::Real;

FixtureAnchor fixtureAnchor{};
bool fixtureAnchorLoaded = false;

class Lock {
public:
    explicit Lock(TickType_t timeoutTicks = portMAX_DELAY)
        : locked(mutex && xSemaphoreTakeRecursive(mutex, timeoutTicks) == pdTRUE) {}
    ~Lock() { if (locked) xSemaphoreGiveRecursive(mutex); }
    explicit operator bool() const { return locked; }
private:
    bool locked;
};

const char* datasetDir(Dataset dataset) {
    return dataset == Dataset::Demo ? kDemoDir : kRealDir;
}

char datasetMarker(Dataset dataset) {
    return dataset == Dataset::Demo ? 'd' : 'r';
}

bool isFixture(const CatalogEntry& entry) {
    return entry.sessionId == 0;
}

void invalidateCatalog() {
    catalogValid = false;
    catalogCount = 0;
}

void makeOpenName(char* out, size_t size, Dataset dataset,
                  uint32_t session, uint32_t firstMinute) {
    snprintf(out, size, "h1-%c-s%010lu-m%010lu.open", datasetMarker(dataset),
             static_cast<unsigned long>(session), static_cast<unsigned long>(firstMinute));
}

void makeClosedName(char* out, size_t size, Dataset dataset, uint32_t session,
                    uint32_t firstMinute, uint16_t count) {
    snprintf(out, size, "h1-%c-s%010lu-m%010lu-n%04u.bin", datasetMarker(dataset),
             static_cast<unsigned long>(session), static_cast<unsigned long>(firstMinute), count);
}

bool parseName(const char* name, Dataset expectedDataset, CatalogEntry& entry) {
    unsigned long session = 0, minute = 0;
    unsigned count = 0;
    char marker = '\0';
    int consumed = 0;
    const bool closed = sscanf(name, "h1-%c-s%10lu-m%10lu-n%4u.bin%n",
                               &marker, &session, &minute, &count, &consumed) == 4 &&
                        name[consumed] == '\0' && count <= kRecordsPerSegment;
    if (!closed) {
        consumed = 0;
        if (sscanf(name, "h1-%c-s%10lu-m%10lu.open%n", &marker, &session, &minute,
                   &consumed) != 3 || name[consumed] != '\0') return false;
    }
    if (marker != datasetMarker(expectedDataset)) return false;
    if (session == 0 && expectedDataset != Dataset::Demo) return false;
    char canonical[52];
    if (closed) {
        makeClosedName(canonical, sizeof(canonical), expectedDataset,
                       static_cast<uint32_t>(session), static_cast<uint32_t>(minute),
                       static_cast<uint16_t>(count));
    } else {
        makeOpenName(canonical, sizeof(canonical), expectedDataset,
                     static_cast<uint32_t>(session), static_cast<uint32_t>(minute));
    }
    if (strcmp(name, canonical) != 0) return false;
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

void fullPath(char* out, size_t size, Dataset dataset, const char* name) {
    snprintf(out, size, "%s/%s", datasetDir(dataset), name);
}

size_t buildCatalog(Dataset dataset, CatalogEntry* out, size_t capacity) {
    if (out == catalogScratch && catalogValid && catalogDataset == dataset) return catalogCount;
    size_t count = 0;
    File directory = LittleFS.open(datasetDir(dataset));
    if (directory && directory.isDirectory()) {
        File file = directory.openNextFile();
        while (file) {
            CatalogEntry entry{};
            const char* full = file.name();
            const char* base = strrchr(full, '/');
            base = base ? base + 1 : full;
            if (!file.isDirectory() && parseName(base, dataset, entry) && count < capacity) {
                entry.bytes = file.size();
                const uint32_t completeRows = entry.bytes / sizeof(MinuteEnergyRecord);
                if ((!entry.closed || (completeRows == entry.records &&
                                       entry.bytes % sizeof(MinuteEnergyRecord) == 0)) &&
                    completeRows <= kRecordsPerSegment) {
                    if (!entry.closed) entry.records = static_cast<uint16_t>(completeRows);
                    entry.active = activeExists && haveRecordingDataset &&
                                   recordingDataset == dataset && strcmp(entry.name, activeName) == 0;
                    out[count++] = entry;
                }
            }
            file = directory.openNextFile();
        }
        directory.close();
    }

    // A newly allocated active segment exists in RAM before its first flash
    // append and therefore is not visible in LittleFS yet.
    if (activeExists && haveRecordingDataset && recordingDataset == dataset) {
        bool found = false;
        for (size_t i = 0; i < count; ++i) {
            if (strcmp(out[i].name, activeName) == 0) {
                out[i].active = true;
                found = true;
                break;
            }
        }
        if (!found && count < capacity) {
            CatalogEntry entry{};
            if (parseName(activeName, dataset, entry)) {
                entry.active = true;
                out[count++] = entry;
            }
        }
    }

    std::sort(out, out + count, catalogLess);
    if (out == catalogScratch) {
        catalogDataset = dataset;
        catalogCount = count;
        catalogValid = true;
    }
    return count;
}

int catalogIndex(Dataset dataset, const char* name) {
    if (!catalogValid || catalogDataset != dataset) buildCatalog(dataset, catalogScratch, kCatalogCapacity);
    for (size_t i = 0; i < catalogCount; ++i) {
        if (strcmp(catalogScratch[i].name, name) == 0) return static_cast<int>(i);
    }
    return -1;
}

bool enforceRetention(Dataset dataset) {
    CatalogEntry* catalog = catalogScratch;
    size_t count = buildCatalog(dataset, catalog, kCatalogCapacity);
    while (count >= kMaxHistoryFiles) {
        size_t victim = 0;
        while (victim < count && (catalog[victim].active || isFixture(catalog[victim]))) ++victim;
        if (victim == count) return false;
        char path[96];
        fullPath(path, sizeof(path), dataset, catalog[victim].name);
        if (!LittleFS.remove(path)) return false;
        memmove(catalog + victim, catalog + victim + 1,
                (count - victim - 1) * sizeof(CatalogEntry));
        --count;
    }
    catalogDataset = dataset;
    catalogCount = count;
    catalogValid = true;
    return true;
}

bool ensureActive() {
    if (activeExists) return true;
    if (!haveRecordingDataset || !ramCount || !enforceRetention(recordingDataset)) return false;
    const uint32_t sessionId = time_service::currentSessionId();
    if (!sessionId) return false; // Session zero is reserved for Demo fixtures.
    activeFirstMinute = ramFirstMinute;
    makeOpenName(activeName, sizeof(activeName), recordingDataset,
                 sessionId, activeFirstMinute);
    activeCommitted = 0;
    activeExists = true;
    invalidateCatalog();
    buildCatalog(recordingDataset, catalogScratch, kCatalogCapacity);
    return true;
}

bool closeActive() {
    if (!activeExists || activeCommitted != kRecordsPerSegment || ramCount) return false;
    char oldPath[96], newPath[96], closedName[52];
    fullPath(oldPath, sizeof(oldPath), recordingDataset, activeName);
    makeClosedName(closedName, sizeof(closedName), recordingDataset,
                   time_service::currentSessionId(), activeFirstMinute, activeCommitted);
    fullPath(newPath, sizeof(newPath), recordingDataset, closedName);
    if (!LittleFS.rename(oldPath, newPath)) return false;
    activeExists = false;
    activeName[0] = '\0';
    activeCommitted = 0;
    invalidateCatalog();
    return true;
}

bool flushRam() {
    if (!ramCount) return true;
    if (!ensureActive()) return false;
    char path[96];
    fullPath(path, sizeof(path), recordingDataset, activeName);
    File file = LittleFS.open(path, "a");
    if (!file) return false;
    const size_t bytes = ramCount * sizeof(MinuteEnergyRecord);
    const bool ok = file.write(reinterpret_cast<const uint8_t*>(ram), bytes) == bytes;
    file.flush();
    file.close();
    if (!ok) return false;
    activeCommitted += ramCount;
    ramCount = 0;
    invalidateCatalog();
    if (activeCommitted == kRecordsPerSegment) return closeActive();
    return activeCommitted < kRecordsPerSegment;
}

void resetMinuteAccumulators() {
    memset(channelEnergyAccumWh, 0, sizeof(channelEnergyAccumWh));
    memset(componentEnergyAccumWh, 0, sizeof(componentEnergyAccumWh));
    memset(channelCoverageAccumMs, 0, sizeof(channelCoverageAccumMs));
    memset(componentCoverageAccumMs, 0, sizeof(componentCoverageAccumMs));
    minuteConfiguredMask = 0;
    minuteQualityFlags = QUALITY_NONE;
}

bool stopActiveRun() {
    if (ramCount && !flushRam()) {
        Serial.println("historical_storage: could not commit pending V1 rows at run boundary");
        return false;
    }
    // A partial .open file is intentionally left as interruption evidence.
    activeExists = false;
    activeName[0] = '\0';
    activeCommitted = 0;
    invalidateCatalog();
    runBreakPending = false;
    return true;
}

void appendCompletedMinute(uint32_t minute) {
    if (runBreakPending && !stopActiveRun()) {
        // Do not append a non-contiguous row to the previous filename. The
        // current minute is an honest gap while the older batch remains in RAM.
        resetMinuteAccumulators();
        return;
    }
    if (!minuteConfiguredMask) {
        if (!stopActiveRun()) runBreakPending = true;
        resetMinuteAccumulators();
        return;
    }
    if (ramCount == kRamCapacity) {
        Serial.println("historical_storage: V1 RAM segment full; flash append is failing");
        resetMinuteAccumulators();
        return;
    }
    if (!ramCount) ramFirstMinute = minute;
    MinuteEnergyRecord& record = ram[ramCount++];
    memset(&record, 0, sizeof(record));
    for (uint8_t i = 0; i < kSensorCount; ++i) {
        record.channelEnergyWh[i] = static_cast<float>(channelEnergyAccumWh[i]);
        record.channelCoverageMs[i] = static_cast<uint16_t>(
            std::min<uint32_t>(channelCoverageAccumMs[i], 60000));
    }
    for (uint8_t i = 0; i < COMPONENT_COUNT; ++i) {
        record.componentEnergyWh[i] = static_cast<float>(componentEnergyAccumWh[i]);
        record.componentCoverageMs[i] = static_cast<uint16_t>(
            std::min<uint32_t>(componentCoverageAccumMs[i], 60000));
    }
    record.configuredChannelMask = minuteConfiguredMask;
    record.qualityFlags = minuteQualityFlags;
    resetMinuteAccumulators();

    if (ramCount == 1 && !ensureActive()) {
        Serial.println("historical_storage: active V1 segment metadata unavailable");
    }
    if (ramCount >= kFlushIntervalMinutes || activeCommitted + ramCount == kRecordsPerSegment) {
        if (!flushRam()) Serial.println("historical_storage: retaining unwritten V1 rows in RAM");
    }
}

bool switchRecordingDataset(Dataset dataset) {
    if (!haveRecordingDataset) {
        recordingDataset = dataset;
        haveRecordingDataset = true;
        return true;
    }
    if (recordingDataset == dataset) return true;
    if (!stopActiveRun()) {
        runBreakPending = true;
        return false;
    }
    recordingDataset = dataset;
    boundaryInitialized = false;
    haveLastFrame = false;
    resetMinuteAccumulators();
    return true;
}

bool readRecord(Dataset dataset, const CatalogEntry& entry, uint16_t index,
                File& file, LocatedRecord& out) {
    const bool runtimeActive = entry.active && haveRecordingDataset && recordingDataset == dataset;
    if (index >= entry.records + (runtimeActive ? ramCount : 0)) return false;
    if (index < entry.records) {
        if (!file) {
            char path[96];
            fullPath(path, sizeof(path), dataset, entry.name);
            file = LittleFS.open(path, "r");
        }
        if (!file || !file.seek(static_cast<uint32_t>(index) * sizeof(MinuteEnergyRecord)) ||
            file.read(reinterpret_cast<uint8_t*>(&out.energy), sizeof(out.energy)) != sizeof(out.energy)) {
            return false;
        }
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

void clearBucket(PowerBucket& bucket) {
    memset(&bucket, 0, sizeof(bucket));
}

void addRecord(PowerBucket& bucket, const MinuteEnergyRecord& record, uint32_t overlapMs) {
    const double fraction = static_cast<double>(overlapMs) / kMinuteMs;
    for (uint8_t i = 0; i < kSensorCount; ++i) {
        bucket.energyWh[i] += record.channelEnergyWh[i] * fraction;
        bucket.channelCoverageMs[i] += static_cast<uint32_t>(record.channelCoverageMs[i] * fraction);
    }
    for (uint8_t i = 0; i < COMPONENT_COUNT; ++i) {
        bucket.componentEnergyWh[i] += record.componentEnergyWh[i] * fraction;
        bucket.componentCoverageMs[i] += static_cast<uint32_t>(record.componentCoverageMs[i] * fraction);
    }
    bucket.configuredChannelMask |= record.configuredChannelMask;
    bucket.qualityFlags |= record.qualityFlags;
}

void finishBucket(PowerBucket& bucket, uint32_t elapsedMs) {
    if (!bucket.durationMinutes) return;
    for (uint8_t i = 0; i < COMPONENT_COUNT; ++i) {
        bucket.componentAveragePowerW[i] = bucket.componentCoverageMs[i]
            ? bucket.componentEnergyWh[i] * 3600000.0f / bucket.componentCoverageMs[i]
            : NAN;
    }
    const uint32_t expected = elapsedMs;
    if (expected > bucket.coveredMs + kMaterialGapMs) bucket.timeFlags |= TIME_INCOMPLETE;
    for (uint8_t i = 0; i < kSensorCount; ++i) {
        if ((bucket.configuredChannelMask & (1U << i)) &&
            expected > bucket.channelCoverageMs[i] + kMaterialGapMs) {
            bucket.timeFlags |= TIME_INCOMPLETE;
        }
    }
}

bool directStart(const CatalogEntry& entry, int64_t& startMs) {
    return entry.sessionId != 0 && time_service::resolveUnixTimeMs(
        entry.sessionId, static_cast<uint64_t>(entry.firstMinute) * kMinuteUs, startMs);
}

int64_t localMidnightUtc(int64_t unixMs, int16_t offsetMinutes) {
    const int64_t offsetMs = static_cast<int64_t>(offsetMinutes) * kMinuteMs;
    return ((unixMs + offsetMs) / kDayMs) * kDayMs - offsetMs;
}

bool sessionBounds(const CatalogEntry* catalog, size_t count, uint32_t session,
                   uint32_t& firstMinute, uint32_t& endMinute) {
    bool found = false;
    firstMinute = UINT32_MAX;
    endMinute = 0;
    for (size_t i = 0; i < count; ++i) {
        if (catalog[i].sessionId != session) continue;
        found = true;
        firstMinute = std::min(firstMinute, catalog[i].firstMinute);
        const uint32_t buffered = catalog[i].active ? ramCount : 0;
        endMinute = std::max(endMinute, catalog[i].firstMinute + catalog[i].records + buffered);
    }
    return found;
}

bool loadFixtureAnchor() {
    if (fixtureAnchorLoaded) return true;
    File file = LittleFS.open(kFixtureAnchorPath, "r");
    if (!file) return false;
    FixtureAnchor loaded{};
    const bool ok = file.read(reinterpret_cast<uint8_t*>(&loaded), sizeof(loaded)) == sizeof(loaded) &&
                    loaded.magic == kFixtureAnchorMagic && loaded.version == kFixtureVersion;
    file.close();
    if (!ok) return false;
    fixtureAnchor = loaded;
    fixtureAnchorLoaded = true;
    return true;
}

bool saveFixtureAnchor(const FixtureAnchor& anchor) {
    LittleFS.remove(kFixtureAnchorTempPath);
    File file = LittleFS.open(kFixtureAnchorTempPath, "w");
    if (!file) return false;
    const bool wrote = file.write(reinterpret_cast<const uint8_t*>(&anchor), sizeof(anchor)) == sizeof(anchor);
    file.flush();
    file.close();
    if (!wrote) {
        LittleFS.remove(kFixtureAnchorTempPath);
        return false;
    }
    LittleFS.remove(kFixtureAnchorPath);
    if (!LittleFS.rename(kFixtureAnchorTempPath, kFixtureAnchorPath)) return false;
    fixtureAnchor = anchor;
    fixtureAnchorLoaded = true;
    return true;
}

bool ensureFixtureAnchor(const CatalogEntry* catalog, size_t count) {
    if (loadFixtureAnchor()) return true;
    if (!time_service::hasCurrentTime()) return false;
    int64_t nowMs = 0;
    if (!time_service::resolveUnixTimeMs(time_service::currentSessionId(),
            time_service::monotonicUs(), nowMs)) return false;

    // First require every recorded Demo segment to be directly anchored. Do
    // not allocate an interval list on the bounded history-worker stack.
    for (size_t i = 0; i < count; ++i) {
        if (isFixture(catalog[i])) continue;
        int64_t start = 0;
        // Do not guess fixture placement around recorded Demo data whose wall
        // time is still unresolved. A later query can pin it once anchored.
        if (!directStart(catalog[i], start)) return false;
    }

    const int64_t spanMs = static_cast<int64_t>(kDemoSpanMinutes) * kMinuteMs;
    int64_t candidateStart = ((nowMs - kFixtureAgeMs) / kMinuteMs) * kMinuteMs;
    int64_t candidateEnd = candidateStart + spanMs;
    bool moved = true;
    while (moved) {
        moved = false;
        for (size_t i = 0; i < count; ++i) {
            if (isFixture(catalog[i])) continue;
            int64_t recordedStart = 0;
            if (!directStart(catalog[i], recordedStart)) return false;
            const uint32_t rows = catalog[i].records + (catalog[i].active ? ramCount : 0);
            const int64_t recordedEnd = recordedStart + static_cast<int64_t>(rows) * kMinuteMs;
            if (recordedEnd <= candidateStart || recordedStart >= candidateEnd) continue;
            candidateEnd = localMidnightUtc(recordedStart,
                                             time_service::utcOffsetMinutes());
            candidateStart = candidateEnd - spanMs;
            moved = true;
            break;
        }
    }

    FixtureAnchor anchor{};
    anchor.magic = kFixtureAnchorMagic;
    anchor.version = kFixtureVersion;
    anchor.minuteZeroUnixMs = candidateStart;
    anchor.utcOffsetMinutes = time_service::utcOffsetMinutes();
    return saveFixtureAnchor(anchor);
}

bool resolveFixtureEntry(const CatalogEntry* catalog, size_t count,
                         const CatalogEntry& entry, int64_t& startMs, uint8_t& flags) {
    if (!isFixture(entry) || !ensureFixtureAnchor(catalog, count)) return false;
    startMs = fixtureAnchor.minuteZeroUnixMs + static_cast<int64_t>(entry.firstMinute) * kMinuteMs;
    flags = TIME_ANCHORED;
    return true;
}

bool resolveEntry(Dataset dataset, const CatalogEntry* catalog, size_t count,
                  const CatalogEntry& entry, int64_t& startMs, uint8_t& flags,
                  int64_t maxInferenceUncertaintyMs = 2LL * kInferenceBoundaryMs) {
    if (dataset == Dataset::Demo && resolveFixtureEntry(catalog, count, entry, startMs, flags)) {
        return true;
    }
    if (directStart(entry, startMs)) {
        flags = TIME_ANCHORED;
        return true;
    }
    if (entry.sessionId == 0) return false;

    uint32_t firstMinute = 0, endMinute = 0;
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
        if (session == 0 || session <= previousSession || session >= nextSession ||
            session == lastSession) continue;
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
    const int64_t boundaryGap = slack / (blockSessions + 1);
    const int64_t sessionStart = previousEnd + static_cast<int64_t>(minutesBefore) * kMinuteMs +
                                 boundaryGap * (sessionIndex + 1);
    startMs = sessionStart + static_cast<int64_t>(entry.firstMinute - firstMinute) * kMinuteMs;
    flags = TIME_INFERRED;
    return true;
}

bool overlapsRecordedDemo(const CatalogEntry* catalog, size_t count,
                          int64_t startMs, int64_t endMs) {
    for (size_t i = 0; i < count; ++i) {
        if (isFixture(catalog[i])) continue;
        int64_t recordedStart = 0;
        if (!directStart(catalog[i], recordedStart)) continue;
        const uint32_t rows = catalog[i].records + (catalog[i].active ? ramCount : 0);
        const int64_t recordedEnd = recordedStart + static_cast<int64_t>(rows) * kMinuteMs;
        if (recordedStart < endMs && recordedEnd > startMs) return true;
    }
    return false;
}

uint16_t automaticBucketMinutes(int64_t spanMs) {
    constexpr uint16_t kChoices[] = {2, 5, 10, 15, 30, 60, 120, 240, 360,
                                     720, 1440, 2880, 4320, 10080};
    constexpr uint32_t kTargetBars = 48;
    const uint64_t spanMinutes = static_cast<uint64_t>(std::max<int64_t>(spanMs, kMinuteMs)) /
                                 kMinuteMs;
    const uint64_t target = (spanMinutes + kTargetBars - 1) / kTargetBars;
    for (uint16_t choice : kChoices) if (choice >= target) return choice;
    return kChoices[sizeof(kChoices) / sizeof(kChoices[0]) - 1];
}

bool markerMatches() {
    File file = LittleFS.open(kFixtureMarkerPath, "r");
    if (!file) return false;
    uint32_t version = 0;
    const bool ok = file.read(reinterpret_cast<uint8_t*>(&version), sizeof(version)) == sizeof(version) &&
                    version == kFixtureVersion;
    file.close();
    return ok;
}

bool writeMarker() {
    File file = LittleFS.open(kFixtureMarkerPath, "w");
    if (!file) return false;
    const bool ok = file.write(reinterpret_cast<const uint8_t*>(&kFixtureVersion),
                               sizeof(kFixtureVersion)) == sizeof(kFixtureVersion);
    file.close();
    return ok;
}

bool fixturesMatch(const CatalogEntry* catalog, size_t count) {
    if (!markerMatches()) return false;
    size_t fixtureIndex = 0;
    for (size_t i = 0; i < count; ++i) {
        if (!isFixture(catalog[i])) continue;
        if (fixtureIndex >= kDemoSegmentCount ||
            catalog[i].firstMinute != demo::kFixtureSegments[fixtureIndex].firstMinute ||
            catalog[i].records != demo::kFixtureSegments[fixtureIndex].records ||
            !catalog[i].closed) return false;
        ++fixtureIndex;
    }
    return fixtureIndex == kDemoSegmentCount;
}

bool seedDemoHistory(bool force = false) {
    CatalogEntry* existing = catalogScratch;
    const size_t existingCount = buildCatalog(Dataset::Demo, existing, kCatalogCapacity);
    if (!force && fixturesMatch(existing, existingCount)) return true;

    for (size_t i = 0; i < existingCount; ++i) {
        if (!isFixture(existing[i])) continue;
        char path[96];
        fullPath(path, sizeof(path), Dataset::Demo, existing[i].name);
        if (LittleFS.exists(path) && !LittleFS.remove(path)) return false;
    }
    // Fixture files count toward Demo's bounded directory cap but are never
    // retention victims. Make their reserved space before recreating them.
    size_t recordedCount = 0;
    for (size_t i = 0; i < existingCount; ++i) {
        if (!isFixture(existing[i])) ++recordedCount;
    }
    for (size_t i = 0; recordedCount + kDemoSegmentCount > kMaxHistoryFiles &&
                       i < existingCount; ++i) {
        if (isFixture(existing[i]) || existing[i].active) continue;
        char path[96];
        fullPath(path, sizeof(path), Dataset::Demo, existing[i].name);
        if (LittleFS.exists(path) && !LittleFS.remove(path)) return false;
        --recordedCount;
    }
    LittleFS.remove(kFixtureMarkerPath);
    LittleFS.remove(kFixtureAnchorPath);
    fixtureAnchorLoaded = false;
    invalidateCatalog();

    for (const auto& segment : demo::kFixtureSegments) {
        const uint32_t first = segment.firstMinute;
        const uint16_t records = segment.records;
        char name[52], path[96];
        makeClosedName(name, sizeof(name), Dataset::Demo, 0, first, records);
        fullPath(path, sizeof(path), Dataset::Demo, name);
        File file = LittleFS.open(path, "w");
        if (!file) return false;
        bool ok = true;
        for (uint16_t i = 0; i < records; ++i) {
            const auto& point = demo::kProfile[(first + i) / demo::kProfileStepMinutes];
            const float charge = point.chargeW10 / 10.0f;
            const float use = point.useW10 / 10.0f;
            const float panel = point.panelW10 / 10.0f;
            MinuteEnergyRecord record{};
            record.channelEnergyWh[0] = (charge + panel) / 60.0f;
            record.channelEnergyWh[1] = (use + panel) / 60.0f;
            record.componentEnergyWh[BATTERY_CHARGING] = charge / 60.0f;
            record.componentEnergyWh[BATTERY_USAGE] = use / 60.0f;
            record.componentEnergyWh[PANEL_IN] = panel / 60.0f;
            record.componentEnergyWh[PANEL_USAGE] = panel / 60.0f;
            record.componentEnergyWh[PANEL_SURPLUS] = point.surplusW10 / 600.0f;
            record.configuredChannelMask = 0x03;
            record.channelCoverageMs[0] = 60000;
            record.channelCoverageMs[1] = 60000;
            for (uint8_t component = 0; component < COMPONENT_COUNT; ++component) {
                record.componentCoverageMs[component] = 60000;
            }
            if (file.write(reinterpret_cast<const uint8_t*>(&record), sizeof(record)) != sizeof(record)) {
                ok = false;
                break;
            }
        }
        file.close();
        if (!ok) return false;
    }
    if (!writeMarker()) return false;
    invalidateCatalog();
    return true;
}

size_t queryTimeBuckets(Dataset dataset, PowerBucket* out, size_t maxBuckets,
                        int64_t startMs, int64_t endMs, int64_t nowMs,
                        uint16_t bucketMinutes, QueryStatus* status) {
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
    const size_t files = buildCatalog(dataset, catalog, kCatalogCapacity);
    uint64_t coveredMs = 0, inferredMs = 0;
    const int64_t dataEndMs = std::min(endMs, nowMs);
    for (size_t f = 0; f < files; ++f) {
        int64_t fileStart = 0;
        uint8_t flags = TIME_NONE;
        if (!resolveEntry(dataset, catalog, files, catalog[f], fileStart, flags, bucketMs)) continue;
        const uint16_t rows = catalog[f].records + (catalog[f].active ? ramCount : 0);
        const int64_t fileEnd = fileStart + static_cast<int64_t>(rows) * kMinuteMs;
        if (fileEnd <= startMs || fileStart >= dataEndMs) continue;
        if (dataset == Dataset::Demo && isFixture(catalog[f]) &&
            overlapsRecordedDemo(catalog, files, fileStart, fileEnd)) {
            // This should only occur after a severe wall-time correction.
            // Prefer an explicit fixture gap to double-counting real samples.
            continue;
        }
        const uint16_t firstRow = fileStart < startMs
            ? static_cast<uint16_t>((startMs - fileStart) / kMinuteMs) : 0;
        const uint16_t lastRow = static_cast<uint16_t>(std::min<int64_t>(
            rows, (dataEndMs - fileStart + kMinuteMs - 1) / kMinuteMs));
        if (lastRow > firstRow && status) ++status->filesRead;
        File file;
        for (uint16_t r = firstRow; r < lastRow; ++r) {
            LocatedRecord record{};
            if (!readRecord(dataset, catalog[f], r, file, record)) continue;
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
                addRecord(out[b], record.energy, overlap);
                out[b].coveredMs += overlap;
                out[b].timeFlags |= flags;
                if (!out[b].startSequence) out[b].startSequence = sequenceOf(record);
                coveredMs += overlap;
                if (flags & TIME_INFERRED) inferredMs += overlap;
            }
            if ((r & 0x3fU) == 0x3fU) vTaskDelay(pdMS_TO_TICKS(1));
        }
        if (file) file.close();
    }
    bool measurementIncomplete = false;
    for (size_t i = 0; i < bucketCount; ++i) {
        const int64_t bucketEnd = out[i].startUnixMs +
                                  static_cast<int64_t>(out[i].durationMinutes) * kMinuteMs;
        const uint32_t elapsedMs = dataEndMs > out[i].startUnixMs
            ? static_cast<uint32_t>(std::min<int64_t>(bucketEnd, dataEndMs) - out[i].startUnixMs)
            : 0;
        finishBucket(out[i], elapsedMs);
        measurementIncomplete |= elapsedMs && (out[i].timeFlags & TIME_INCOMPLETE) != 0;
    }

    const uint64_t elapsedSpan = dataEndMs > startMs ? dataEndMs - startMs : 0;
    const uint64_t missing = elapsedSpan > coveredMs ? elapsedSpan - coveredMs : 0;
    if (status) {
        status->startUnixMs = startMs;
        status->endUnixMs = endMs;
        status->coveredMinutes = coveredMs / kMinuteMs;
        status->missingMinutes = missing / kMinuteMs;
        status->inferredMinutes = inferredMs / kMinuteMs;
        status->incomplete = missing > kMaterialGapMs || measurementIncomplete;
        status->hasInferredTime = inferredMs != 0;
    }
    return bucketCount;
}

} // namespace

Dataset activeDataset() {
    return sensor_mode::get() == sensor_mode::Mode::Demo ? Dataset::Demo : Dataset::Real;
}

bool init() {
    if (!mutex) mutex = xSemaphoreCreateRecursiveMutex();
    Lock lock;
    if (!lock) return false;
    if (!LittleFS.begin(true)) {
        Serial.println("historical_storage: LittleFS mount failed");
        return false;
    }
    if (!LittleFS.exists(kHistoryDir)) LittleFS.mkdir(kHistoryDir);
    if (!LittleFS.exists(kV1Dir)) LittleFS.mkdir(kV1Dir);
    if (!LittleFS.exists(kRealDir)) LittleFS.mkdir(kRealDir);
    if (!LittleFS.exists(kDemoDir)) LittleFS.mkdir(kDemoDir);
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
    if (!seedDemoHistory()) Serial.println("historical_storage: demo fixture seed incomplete");
    invalidateCatalog();
    const size_t realFiles = buildCatalog(Dataset::Real, catalogScratch, kCatalogCapacity);
    invalidateCatalog();
    const size_t demoFiles = buildCatalog(Dataset::Demo, catalogScratch, kCatalogCapacity);
    Serial.printf("historical_storage: V1 ready, %u Real and %u Demo files\n",
                  static_cast<unsigned>(realFiles), static_cast<unsigned>(demoFiles));
    return true;
}

void addSampleFrame(Dataset dataset, const SampleFrame& frame) {
    Lock lock(0);
    if (!lock || !ready) return;
    if (!switchRecordingDataset(dataset)) return;
    if (!boundaryInitialized) {
        lastFrame = frame;
        haveLastFrame = true;
        return;
    }
    if (haveLastFrame) {
        const uint32_t dtMs = frame.timestampMs - lastFrame.timestampMs;
        if (dtMs && dtMs <= kMaxFrameIntervalMs) {
            const uint8_t channelMask = lastFrame.eligibleChannelMask & frame.eligibleChannelMask;
            const uint8_t componentMask = lastFrame.eligibleComponentMask & frame.eligibleComponentMask;
            uint32_t elapsedMs = 0;
            while (elapsedMs < dtMs) {
                const uint32_t cursorMs = lastFrame.timestampMs + elapsedMs;
                uint32_t toBoundaryMs = nextBoundaryTimestampMs - cursorMs;
                if (toBoundaryMs == 0) {
                    appendCompletedMinute(nextBoundaryMinute - 1);
                    ++nextBoundaryMinute;
                    nextBoundaryTimestampMs += 60000U;
                    continue;
                }
                const uint32_t partMs = std::min(dtMs - elapsedMs, toBoundaryMs);
                const double startFraction = static_cast<double>(elapsedMs) / dtMs;
                const double endFraction = static_cast<double>(elapsedMs + partMs) / dtMs;
                const double hours = static_cast<double>(partMs) / 3600000.0;
                minuteConfiguredMask |= lastFrame.configuredChannelMask |
                                        frame.configuredChannelMask;
                minuteQualityFlags |= lastFrame.qualityFlags | frame.qualityFlags;
                for (uint8_t i = 0; i < kSensorCount; ++i) {
                    if (!(channelMask & (1U << i)) ||
                        !std::isfinite(lastFrame.channelPowerW[i]) ||
                        !std::isfinite(frame.channelPowerW[i])) continue;
                    const double delta = static_cast<double>(frame.channelPowerW[i]) -
                                         lastFrame.channelPowerW[i];
                    const double startPower = lastFrame.channelPowerW[i] + delta * startFraction;
                    const double endPower = lastFrame.channelPowerW[i] + delta * endFraction;
                    channelEnergyAccumWh[i] += (startPower + endPower) * 0.5 * hours;
                    channelCoverageAccumMs[i] += partMs;
                }
                for (uint8_t i = 0; i < COMPONENT_COUNT; ++i) {
                    if (!(componentMask & (1U << i)) ||
                        !std::isfinite(lastFrame.componentPowerW[i]) ||
                        !std::isfinite(frame.componentPowerW[i])) continue;
                    const double delta = static_cast<double>(frame.componentPowerW[i]) -
                                         lastFrame.componentPowerW[i];
                    const double startPower = lastFrame.componentPowerW[i] + delta * startFraction;
                    const double endPower = lastFrame.componentPowerW[i] + delta * endFraction;
                    componentEnergyAccumWh[i] += (startPower + endPower) * 0.5 * hours;
                    componentCoverageAccumMs[i] += partMs;
                }
                elapsedMs += partMs;
                if (partMs == toBoundaryMs) {
                    appendCompletedMinute(nextBoundaryMinute - 1);
                    ++nextBoundaryMinute;
                    nextBoundaryTimestampMs += 60000U;
                }
            }
        } else if (frame.configuredChannelMask || lastFrame.configuredChannelMask) {
            minuteQualityFlags |= QUALITY_STALE_OR_MISSING;
        }
    }
    // A point observation belongs to the minute after an exact boundary and
    // ensures a configured-but-invalid source still emits an explicit gap row.
    minuteConfiguredMask |= frame.configuredChannelMask;
    minuteQualityFlags |= frame.qualityFlags;
    lastFrame = frame;
    haveLastFrame = true;
}

void tick() {
    Lock lock(0);
    if (!lock || !ready) return;
    const uint64_t monotonicUs = time_service::monotonicUs();
    const uint32_t minute = static_cast<uint32_t>(monotonicUs / kMinuteUs);
    if (!boundaryInitialized) {
        nextBoundaryMinute = minute + 1;
        const uint64_t remainingUs = static_cast<uint64_t>(nextBoundaryMinute) * kMinuteUs -
                                     monotonicUs;
        nextBoundaryTimestampMs = millis() + static_cast<uint32_t>((remainingUs + 999) / 1000);
        resetMinuteAccumulators();
        haveLastFrame = false;
        boundaryInitialized = true;
        return;
    }
    if (minute < nextBoundaryMinute) return;
    // Give the producer time to deliver the observation that brackets this
    // boundary. addSampleFrame() can then split that interval exactly. Once
    // the accepted interval has expired, close a partial minute and never
    // bridge the eventual late sample across it.
    if (static_cast<uint32_t>(millis() - nextBoundaryTimestampMs) < kBoundaryGraceMs) return;
    if (minuteConfiguredMask) minuteQualityFlags |= QUALITY_STALE_OR_MISSING;
    appendCompletedMinute(nextBoundaryMinute - 1);
    haveLastFrame = false;
    if (minute > nextBoundaryMinute) {
        // Missing whole minutes are gaps, not zero-valued rows. End the current
        // contiguous segment so filename position remains truthful.
        if (!stopActiveRun()) runBreakPending = true;
    }
    nextBoundaryMinute = minute + 1;
    const uint64_t remainingUs = static_cast<uint64_t>(nextBoundaryMinute) * kMinuteUs -
                                 monotonicUs;
    nextBoundaryTimestampMs = millis() + static_cast<uint32_t>((remainingUs + 999) / 1000);
}

size_t getPowerBucketsForDataset(Dataset dataset, PowerBucket* out, size_t maxBuckets,
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
    return queryTimeBuckets(dataset, out, maxBuckets, startMs, endMs, nowMs,
                            bucketMinutes, status);
}

size_t getPowerBuckets(PowerBucket* out, size_t maxBuckets,
                       uint32_t lookbackMinutes, uint16_t bucketMinutes,
                       uint32_t endOffsetMinutes, bool includePartial,
                       QueryStatus* status) {
    return getPowerBucketsForDataset(activeDataset(), out, maxBuckets, lookbackMinutes,
                                     bucketMinutes, endOffsetMinutes, includePartial, status);
}

size_t getCalendarPowerBucketsForDataset(Dataset dataset, PowerBucket* out, size_t maxBuckets,
                                         CalendarRange range, uint16_t bucketMinutes,
                                         QueryStatus* status) {
    Lock lock;
    if (status) *status = {};
    if (!lock || !ready || !out || !maxBuckets ||
        (!bucketMinutes && range != CalendarRange::All) || !time_service::hasCurrentTime()) return 0;
    int64_t nowMs = 0;
    if (!time_service::resolveUnixTimeMs(time_service::currentSessionId(),
            time_service::monotonicUs(), nowMs)) return 0;
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
            const size_t files = buildCatalog(dataset, catalog, kCatalogCapacity);
            bool found = false;
            int64_t oldest = 0;
            const int64_t tolerance = static_cast<int64_t>(bucketMinutes) * kMinuteMs;
            for (size_t i = 0; i < files; ++i) {
                int64_t resolved = 0;
                uint8_t flags = TIME_NONE;
                if (resolveEntry(dataset, catalog, files, catalog[i], resolved, flags, tolerance) &&
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
    return queryTimeBuckets(dataset, out, maxBuckets, startMs, endMs, nowMs,
                            bucketMinutes, status);
}

size_t getCalendarPowerBuckets(PowerBucket* out, size_t maxBuckets,
                               CalendarRange range, uint16_t bucketMinutes,
                               QueryStatus* status) {
    return getCalendarPowerBucketsForDataset(activeDataset(), out, maxBuckets,
                                             range, bucketMinutes, status);
}

size_t listFilesForDataset(Dataset dataset, HistoryFileInfo* out, size_t limit,
                           size_t offset, size_t* total, StorageStats* stats) {
    Lock lock;
    if (!lock || !ready) {
        if (total) *total = 0;
        return 0;
    }
    CatalogEntry* catalog = catalogScratch;
    const size_t count = buildCatalog(dataset, catalog, kCatalogCapacity);
    if (stats) {
        *stats = {};
        stats->maxFiles = kMaxHistoryFiles;
        for (size_t i = 0; i < count; ++i) {
            ++stats->fileCount;
            if (isFixture(catalog[i])) ++stats->fixtureFileCount;
            stats->committedRecords += catalog[i].records;
            stats->committedBytes += catalog[i].bytes;
        }
        if (haveRecordingDataset && recordingDataset == dataset) {
            stats->bufferedRecords = static_cast<uint8_t>(ramCount);
            stats->bufferedBytes = ramCount * sizeof(MinuteEnergyRecord);
        }
    }
    if (total) *total = count;
    if (!out || !limit || offset >= count) return 0;
    size_t resultCount = 0;
    size_t skipped = 0;
    for (size_t index = count; index > 0 && resultCount < limit; --index) {
        const CatalogEntry& entry = catalog[index - 1];
        if (skipped++ < offset) continue;
        HistoryFileInfo info{};
        strncpy(info.name, entry.name, sizeof(info.name) - 1);
        info.sessionId = entry.sessionId;
        info.firstMinute = entry.firstMinute;
        info.committedRecords = entry.records;
        info.bufferedRecords = entry.active ? static_cast<uint8_t>(ramCount) : 0;
        info.state = entry.active ? FileState::Active :
                     (entry.closed ? FileState::Closed : FileState::Interrupted);
        info.bytes = entry.bytes;
        info.fixture = isFixture(entry);
        int64_t start = 0;
        uint8_t flags = TIME_NONE;
        if (resolveEntry(dataset, catalog, count, entry, start, flags)) {
            info.startUnixMs = start;
            info.endUnixMs = start + static_cast<int64_t>(
                entry.records + (entry.active ? ramCount : 0)) * kMinuteMs;
            info.timeFlags = flags;
        }
        out[resultCount++] = info;
    }
    return resultCount;
}

size_t listFiles(HistoryFileInfo* out, size_t limit, size_t offset,
                 size_t* total, StorageStats* stats) {
    return listFilesForDataset(activeDataset(), out, limit, offset, total, stats);
}

void getStorageStatsForDataset(Dataset dataset, StorageStats& out) {
    listFilesForDataset(dataset, nullptr, 0, 0, nullptr, &out);
}

void getStorageStats(StorageStats& out) {
    getStorageStatsForDataset(activeDataset(), out);
}

bool clearDataset(Dataset dataset) {
    Lock lock;
    if (!lock || !ready) return false;
    if (haveRecordingDataset && recordingDataset == dataset) {
        ramCount = 0;
        activeExists = false;
        activeName[0] = '\0';
        activeCommitted = 0;
        boundaryInitialized = false;
        haveLastFrame = false;
        runBreakPending = false;
        resetMinuteAccumulators();
    }
    bool ok = true;
    CatalogEntry* catalog = catalogScratch;
    const size_t count = buildCatalog(dataset, catalog, kCatalogCapacity);
    for (size_t i = 0; i < count; ++i) {
        char path[96];
        fullPath(path, sizeof(path), dataset, catalog[i].name);
        if (LittleFS.exists(path) && !LittleFS.remove(path)) ok = false;
    }
    invalidateCatalog();
    if (dataset == Dataset::Demo) {
        LittleFS.remove(kFixtureMarkerPath);
        LittleFS.remove(kFixtureAnchorPath);
        fixtureAnchorLoaded = false;
        ok = seedDemoHistory(true) && ok;
    }
    return ok;
}

bool clearAll() {
    return clearDataset(activeDataset());
}

bool clearAllDatasets() {
    Lock lock;
    if (!lock || !ready) return false;
    bool ok = clearDataset(Dataset::Real);
    ok = clearDataset(Dataset::Demo) && ok;
    ok = time_service::clearHistoryAnchors() && ok;
    return ok;
}

size_t recordCountForDataset(Dataset dataset) {
    StorageStats stats{};
    getStorageStatsForDataset(dataset, stats);
    return stats.committedRecords + stats.bufferedRecords;
}

size_t recordCount() {
    return recordCountForDataset(activeDataset());
}

} // namespace historical_storage
