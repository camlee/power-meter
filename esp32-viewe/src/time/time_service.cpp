#include "time_service.h"

#include <Arduino.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <sys/time.h>
#include <time.h>

namespace time_service {
namespace {

constexpr char kPreferencesNamespace[] = "time_service";
constexpr char kSessionKey[] = "session";
constexpr char kOffsetKey[] = "utc_offset";
constexpr char kLedgerPath[] = "/history/v1/time-anchors.bin";
constexpr char kLedgerTempPath[] = "/history/v1/time-anchors.tmp";
constexpr char kLedgerBackupPath[] = "/history/v1/time-anchors.bak";
constexpr int16_t kDefaultUtcOffsetMinutes = -7 * 60;
constexpr int16_t kMinUtcOffsetMinutes = -14 * 60;
constexpr int16_t kMaxUtcOffsetMinutes = 14 * 60;
constexpr int64_t kEarliestAcceptedUnixMs = 1577836800000LL;
constexpr uint32_t kLedgerMagic = 0x33415448; // HTA3 on little-endian ESP32
constexpr uint16_t kLedgerVersion = 1;
constexpr size_t kMaxRetainedAnchors = 200;

#pragma pack(push, 1)
struct LedgerHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t count;
};
struct LedgerAnchor {
    uint32_t sessionId;
    uint64_t monotonicMs;
    int64_t unixTimeMs;
    uint32_t uncertaintyMs;
    int16_t utcOffsetMinutes;
    uint8_t source;
    uint8_t reserved;
};
#pragma pack(pop)
static_assert(sizeof(LedgerAnchor) == 28, "anchor ledger rows must stay fixed-size");

SemaphoreHandle_t mutex = nullptr;
bool initialized = false;
uint32_t sessionId = 0;
int16_t configuredUtcOffsetMinutes = kDefaultUtcOffsetMinutes;
LedgerAnchor anchors[kMaxRetainedAnchors]{};
uint16_t anchorCount = 0;
bool currentAnchorValid = false;
Anchor currentAnchor{};

class Lock {
public:
    Lock() : locked(mutex && xSemaphoreTake(mutex, portMAX_DELAY) == pdTRUE) {}
    ~Lock() { if (locked) xSemaphoreGive(mutex); }
    explicit operator bool() const { return locked; }
private:
    bool locked;
};

bool offsetIsValid(int16_t value) {
    return value >= kMinUtcOffsetMinutes && value <= kMaxUtcOffsetMinutes;
}

void applyFixedTimezone(int16_t offsetMinutes) {
    const int reversed = -static_cast<int>(offsetMinutes);
    const int absolute = abs(reversed);
    char timezone[24];
    snprintf(timezone, sizeof(timezone), "UTC%s%d:%02d",
             reversed < 0 ? "-" : "", absolute / 60, absolute % 60);
    setenv("TZ", timezone, 1);
    tzset();
}

uint32_t defaultUncertainty(AnchorSource source, uint32_t supplied) {
    if (supplied) return supplied;
    switch (source) {
        case AnchorSource::Ntp: return 250;
        case AnchorSource::Browser: return 1500;
        case AnchorSource::Peer: return 1000;
        case AnchorSource::Rtc: return 2000;
        default: return 5000;
    }
}

Anchor fromLedger(const LedgerAnchor& value) {
    return Anchor{value.sessionId, value.monotonicMs * 1000ULL, value.unixTimeMs,
                  static_cast<AnchorSource>(value.source), value.utcOffsetMinutes,
                  value.uncertaintyMs};
}

LedgerAnchor toLedger(const Anchor& value) {
    return LedgerAnchor{value.sessionId, value.monotonicUs / 1000ULL,
                        value.unixTimeMs, value.uncertaintyMs,
                        value.utcOffsetMinutes, static_cast<uint8_t>(value.source), 0};
}

int findAnchor(uint32_t wantedSession) {
    for (uint16_t i = 0; i < anchorCount; ++i) {
        if (anchors[i].sessionId == wantedSession) return i;
    }
    return -1;
}

bool loadLedgerFile(const char* path) {
    File file = LittleFS.open(path, "r");
    if (!file) return false;
    LedgerHeader header{};
    const bool headerOk = file.read(reinterpret_cast<uint8_t*>(&header), sizeof(header)) == sizeof(header) &&
        header.magic == kLedgerMagic && header.version == kLedgerVersion &&
        header.count <= kMaxRetainedAnchors &&
        file.size() == sizeof(header) + header.count * sizeof(LedgerAnchor);
    if (!headerOk) { file.close(); return false; }
    const size_t bytes = header.count * sizeof(LedgerAnchor);
    const bool rowsOk = !bytes || file.read(reinterpret_cast<uint8_t*>(anchors), bytes) == bytes;
    file.close();
    if (!rowsOk) return false;
    anchorCount = header.count;
    return true;
}

bool saveLedger() {
    File file = LittleFS.open(kLedgerTempPath, "w");
    if (!file) return false;
    const LedgerHeader header{kLedgerMagic, kLedgerVersion, anchorCount};
    bool ok = file.write(reinterpret_cast<const uint8_t*>(&header), sizeof(header)) == sizeof(header);
    const size_t bytes = anchorCount * sizeof(LedgerAnchor);
    if (ok && bytes) ok = file.write(reinterpret_cast<const uint8_t*>(anchors), bytes) == bytes;
    file.flush();
    file.close();
    if (!ok) { LittleFS.remove(kLedgerTempPath); return false; }

    LittleFS.remove(kLedgerBackupPath);
    if (LittleFS.exists(kLedgerPath) && !LittleFS.rename(kLedgerPath, kLedgerBackupPath)) {
        LittleFS.remove(kLedgerTempPath);
        return false;
    }
    if (!LittleFS.rename(kLedgerTempPath, kLedgerPath)) {
        if (LittleFS.exists(kLedgerBackupPath)) LittleFS.rename(kLedgerBackupPath, kLedgerPath);
        return false;
    }
    LittleFS.remove(kLedgerBackupPath);
    return true;
}

} // namespace

void init() {
    if (!mutex) mutex = xSemaphoreCreateMutex();
    Lock lock;
    if (!lock || initialized) return;

    Preferences preferences;
    if (preferences.begin(kPreferencesNamespace, false)) {
        configuredUtcOffsetMinutes = preferences.getShort(kOffsetKey, kDefaultUtcOffsetMinutes);
        if (!offsetIsValid(configuredUtcOffsetMinutes)) configuredUtcOffsetMinutes = kDefaultUtcOffsetMinutes;
        sessionId = preferences.getUInt(kSessionKey, 0) + 1;
        if (!sessionId) sessionId = 1;
        preferences.putUInt(kSessionKey, sessionId);
        preferences.putShort(kOffsetKey, configuredUtcOffsetMinutes);
        preferences.end();
    } else {
        sessionId = 1;
    }

    if (!loadLedgerFile(kLedgerPath)) {
        anchorCount = 0;
        // Complete an interrupted atomic rewrite when its temporary/backup is valid.
        if (loadLedgerFile(kLedgerTempPath) || loadLedgerFile(kLedgerBackupPath)) saveLedger();
    }
    applyFixedTimezone(configuredUtcOffsetMinutes);
    initialized = true;
    Serial.printf("time_service: session %lu, offset %+d, %u anchors\n",
                  static_cast<unsigned long>(sessionId), configuredUtcOffsetMinutes, anchorCount);
}

uint32_t currentSessionId() {
    if (!initialized) init();
    Lock lock;
    return sessionId;
}

uint64_t monotonicUs() { return static_cast<uint64_t>(esp_timer_get_time()); }

bool submitAnchor(int64_t unixTimeMs, AnchorSource source,
                  int16_t offsetMinutes, uint32_t uncertaintyMs) {
    if (!initialized) init();
    if (unixTimeMs < kEarliestAcceptedUnixMs || source == AnchorSource::Unknown ||
        !offsetIsValid(offsetMinutes)) return false;

    Anchor candidate{sessionId, monotonicUs(), unixTimeMs, source, offsetMinutes,
                     defaultUncertainty(source, uncertaintyMs)};
    Lock lock;
    if (!lock) return false;

    timeval wallClock{static_cast<time_t>(unixTimeMs / 1000),
                      static_cast<suseconds_t>((unixTimeMs % 1000) * 1000)};
    settimeofday(&wallClock, nullptr);
    currentAnchor = candidate;
    currentAnchorValid = true;

    bool persisted = true;
    const bool offsetChanged = configuredUtcOffsetMinutes != offsetMinutes;
    configuredUtcOffsetMinutes = offsetMinutes;
    applyFixedTimezone(offsetMinutes);

    const int index = findAnchor(sessionId);
    bool useful = index < 0;
    if (index >= 0) {
        const LedgerAnchor& old = anchors[index];
        // Ignore clock refreshes unless their declared precision improves by at
        // least 250 ms. This normally leaves exactly one ledger write per boot.
        useful = candidate.uncertaintyMs + 250 <= old.uncertaintyMs;
        if (useful) anchors[index] = toLedger(candidate);
    } else {
        if (anchorCount == kMaxRetainedAnchors) {
            memmove(anchors, anchors + 1, (kMaxRetainedAnchors - 1) * sizeof(LedgerAnchor));
            --anchorCount;
        }
        anchors[anchorCount++] = toLedger(candidate);
    }
    if (useful) persisted = saveLedger();

    if (offsetChanged) {
        Preferences preferences;
        if (preferences.begin(kPreferencesNamespace, false)) {
            persisted &= preferences.putShort(kOffsetKey, offsetMinutes) == sizeof(int16_t);
            preferences.end();
        } else persisted = false;
    }
    Serial.printf("time_service: %s anchor%s (uncertainty %lu ms)%s\n",
                  sourceName(source), useful ? " saved" : " accepted",
                  static_cast<unsigned long>(candidate.uncertaintyMs), persisted ? "" : "; save failed");
    return persisted;
}

bool hasCurrentTime() {
    if (!initialized) init();
    Lock lock;
    return currentAnchorValid;
}

bool getCurrentAnchor(Anchor& out) {
    if (!initialized) init();
    Lock lock;
    if (!lock || !currentAnchorValid) return false;
    out = currentAnchor;
    return true;
}

bool getAnchorForSession(uint32_t wantedSession, Anchor& out) {
    if (!initialized) init();
    Lock lock;
    if (!lock) return false;
    if (wantedSession == sessionId && currentAnchorValid) { out = currentAnchor; return true; }
    const int index = findAnchor(wantedSession);
    if (index < 0) return false;
    out = fromLedger(anchors[index]);
    return true;
}

bool resolveUnixTimeMs(uint32_t wantedSession, uint64_t sampleMonotonicUs,
                       int64_t& unixTimeMsOut, Anchor* anchorOut) {
    Anchor anchor;
    if (!getAnchorForSession(wantedSession, anchor)) return false;
    const int64_t deltaUs = sampleMonotonicUs >= anchor.monotonicUs
        ? static_cast<int64_t>(sampleMonotonicUs - anchor.monotonicUs)
        : -static_cast<int64_t>(anchor.monotonicUs - sampleMonotonicUs);
    unixTimeMsOut = anchor.unixTimeMs + deltaUs / 1000;
    if (anchorOut) *anchorOut = anchor;
    return true;
}

int16_t utcOffsetMinutes() {
    if (!initialized) init();
    Lock lock;
    return configuredUtcOffsetMinutes;
}

bool setUtcOffsetMinutes(int16_t value) {
    if (!initialized) init();
    if (!offsetIsValid(value)) return false;
    Lock lock;
    if (!lock) return false;
    configuredUtcOffsetMinutes = value;
    applyFixedTimezone(value);
    Preferences preferences;
    if (!preferences.begin(kPreferencesNamespace, false)) return false;
    const bool saved = preferences.putShort(kOffsetKey, value) == sizeof(int16_t);
    preferences.end();
    return saved;
}

const char* sourceName(AnchorSource source) {
    switch (source) {
        case AnchorSource::Ntp: return "NTP";
        case AnchorSource::Browser: return "browser";
        case AnchorSource::Peer: return "peer";
        case AnchorSource::Rtc: return "RTC";
        default: return "unknown";
    }
}

bool clearHistoryAnchors() {
    if (!initialized) init();
    Lock lock;
    if (!lock) return false;
    anchorCount = 0;
    currentAnchorValid = false;
    bool ok = true;
    const char* paths[] = {kLedgerPath, kLedgerTempPath, kLedgerBackupPath};
    for (const char* path : paths) if (LittleFS.exists(path) && !LittleFS.remove(path)) ok = false;
    return ok;
}

} // namespace time_service
