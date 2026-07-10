#include "historical_storage.h"
#include <Arduino.h>
#include <LittleFS.h>

namespace historical_storage {
namespace {

constexpr const char* kFilePath = "/historical.bin";
constexpr uint32_t kMagic = 0x48495354; // "HIST"

// 30 days of 1-record-per-minute history by default. At sizeof(MinuteRecord)
// (28 bytes) that's ~1.2MB on internal flash. Lower this -- or swap this
// file for an SD-card-backed implementation behind the same API -- if your
// LittleFS partition is smaller.
constexpr size_t kMaxRecords = 43200;
constexpr size_t kRecordSize = sizeof(MinuteRecord);
constexpr size_t kHeaderSize = 12; // magic + writeIndex + count, each uint32_t

struct Header {
    uint32_t magic;
    uint32_t writeIndex; // slot that will be written to next
    uint32_t count;      // number of valid records currently stored (<= kMaxRecords)
};

Header header{0, 0, 0};
bool ready = false;

// --- In-progress minute accumulator (RAM only, flushed by tick()) --------
bool haveMinuteAnchor = false;
uint32_t anchorMinuteIndex = 0; // millis()/60000 value we last flushed at
double powerSum[kSensorCount] = {0, 0, 0};
uint32_t sampleCount[kSensorCount] = {0, 0, 0};
double energyAccumWh[kSensorCount] = {0, 0, 0};
uint32_t lastSampleTime_ms[kSensorCount] = {0, 0, 0};
bool haveLastSampleTime[kSensorCount] = {false, false, false};

bool readHeader() {
    File f = LittleFS.open(kFilePath, "r");
    if (!f) return false;
    uint32_t buf[3];
    bool ok = f.read((uint8_t*)buf, kHeaderSize) == kHeaderSize;
    f.close();
    if (!ok || buf[0] != kMagic) return false;
    header.magic = buf[0];
    header.writeIndex = buf[1];
    header.count = buf[2];
    return true;
}

void writeHeader() {
    File f = LittleFS.open(kFilePath, "r+");
    if (!f) return;
    uint32_t buf[3] = {header.magic, header.writeIndex, header.count};
    f.seek(0);
    f.write((uint8_t*)buf, kHeaderSize);
    f.close();
}

void createEmptyFile() {
    File f = LittleFS.open(kFilePath, "w");
    if (!f) return;
    header = {kMagic, 0, 0};
    uint32_t buf[3] = {header.magic, header.writeIndex, header.count};
    f.write((uint8_t*)buf, kHeaderSize);
    f.close();
}

// Appends/overwrites one record. While the ring buffer isn't full yet this
// is a plain append (no seeking, so it can't create a sparse-file problem).
// Once full, it overwrites the oldest slot in place -- that offset already
// exists in the file from an earlier append, so seek+write there is safe.
void appendRecord(const MinuteRecord& rec) {
    if (header.count < kMaxRecords) {
        File f = LittleFS.open(kFilePath, "a");
        if (!f) return;
        f.write((const uint8_t*)&rec, kRecordSize);
        f.close();
        header.writeIndex = (header.writeIndex + 1) % kMaxRecords;
        header.count++;
    } else {
        File f = LittleFS.open(kFilePath, "r+");
        if (!f) return;
        size_t offset = kHeaderSize + (size_t)header.writeIndex * kRecordSize;
        f.seek(offset);
        f.write((const uint8_t*)&rec, kRecordSize);
        f.close();
        header.writeIndex = (header.writeIndex + 1) % kMaxRecords;
    }
    writeHeader();
}

} // namespace

bool init() {
    if (!LittleFS.begin(true)) { // format on mount failure (first boot)
        Serial.println("historical_storage: LittleFS mount failed");
        return false;
    }
    if (!LittleFS.exists(kFilePath) || !readHeader()) {
        createEmptyFile();
    }
    ready = true;
    return true;
}

void addSample(uint8_t sensorIndex, float powerW, uint32_t timestamp_ms) {
    if (!ready || sensorIndex >= kSensorCount) return;

    powerSum[sensorIndex] += powerW;
    sampleCount[sensorIndex]++;

    if (haveLastSampleTime[sensorIndex]) {
        uint32_t dt_ms = timestamp_ms - lastSampleTime_ms[sensorIndex];
        energyAccumWh[sensorIndex] += (double)powerW * dt_ms / 3600000.0;
    }
    lastSampleTime_ms[sensorIndex] = timestamp_ms;
    haveLastSampleTime[sensorIndex] = true;
}

void tick() {
    if (!ready) return;

    uint32_t nowMinuteIndex = millis() / 60000;
    if (!haveMinuteAnchor) {
        anchorMinuteIndex = nowMinuteIndex;
        haveMinuteAnchor = true;
        return;
    }
    if (nowMinuteIndex == anchorMinuteIndex) return;

    MinuteRecord rec{};
    rec.epoch_s = anchorMinuteIndex * 60; // seconds since boot; swap for a real epoch once NTP/RTC is added
    for (uint8_t i = 0; i < kSensorCount; i++) {
        rec.avgPowerW[i] = sampleCount[i] > 0 ? (float)(powerSum[i] / sampleCount[i]) : 0.0f;
        rec.energyWh[i] = (float)energyAccumWh[i];
        powerSum[i] = 0;
        sampleCount[i] = 0;
        energyAccumWh[i] = 0;
    }
    appendRecord(rec);
    anchorMinuteIndex = nowMinuteIndex;
}

size_t getRecent(MinuteRecord* out, size_t maxCount) {
    if (!ready) return 0;
    size_t n = header.count < maxCount ? header.count : maxCount;
    if (n == 0) return 0;

    File f = LittleFS.open(kFilePath, "r");
    if (!f) return 0;

    size_t start = (header.writeIndex + kMaxRecords - n) % kMaxRecords;
    for (size_t i = 0; i < n; i++) {
        size_t idx = (start + i) % kMaxRecords;
        size_t offset = kHeaderSize + idx * kRecordSize;
        f.seek(offset);
        f.read((uint8_t*)&out[i], kRecordSize);
    }
    f.close();
    return n;
}

size_t recordCount() {
    return header.count;
}

} // namespace historical_storage
