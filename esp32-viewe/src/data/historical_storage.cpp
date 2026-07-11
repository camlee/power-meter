#include "historical_storage.h"
#include <Arduino.h>
#include <LittleFS.h>

namespace historical_storage {
namespace {

constexpr const char* kRecentFile = "/history_A.bin";
constexpr const char* kArchiveFile = "/history_B.bin";

// Total 30 days = 43200 minutes. We split this evenly across two files.
constexpr size_t kMaxRecordsTotal = 43200;
constexpr size_t kMaxRecordsPerFile = kMaxRecordsTotal / 2;
constexpr size_t kRecordSize = sizeof(MinuteRecord);

// Buffer 1 hour of records in RAM to reduce flash writes by a factor of 60
constexpr size_t kRamBufferSize = 60;
MinuteRecord ramBuffer[kRamBufferSize];
size_t ramBufferCount = 0;

bool ready = false;

// --- In-progress minute accumulator ---------------------------------------
uint32_t lastTick_ms = 0;
uint32_t uptimeMinutes = 0;
bool tickInitialized = false;

double powerSum[kSensorCount] = {0, 0, 0};
uint32_t sampleCount[kSensorCount] = {0, 0, 0};
double energyAccumWh[kSensorCount] = {0, 0, 0};
uint32_t lastSampleTime_ms[kSensorCount] = {0, 0, 0};
bool haveLastSampleTime[kSensorCount] = {false, false, false};


// --- Internal Helpers ----------------------------------------------------

size_t getFileRecordCount(const char* path) {
    File f = LittleFS.open(path, "r");
    if (!f) return 0;
    size_t sz = f.size();
    f.close();
    return sz / kRecordSize;
}

void rotateFiles() {
    // Drop the oldest half of the data, and promote recent to archive
    if (LittleFS.exists(kArchiveFile)) {
        LittleFS.remove(kArchiveFile);
    }
    LittleFS.rename(kRecentFile, kArchiveFile);
}

void flushBuffer() {
    if (ramBufferCount == 0) return;

    File f = LittleFS.open(kRecentFile, "a");
    if (f) {
        // Bulk write the entire array of structs
        f.write((const uint8_t*)ramBuffer, ramBufferCount * kRecordSize);
        f.close();
    }

    ramBufferCount = 0;

    // Check if it's time to rotate
    if (getFileRecordCount(kRecentFile) >= kMaxRecordsPerFile) {
        rotateFiles();
    }
}

} // namespace


bool init() {
    if (!LittleFS.begin(true)) {
        Serial.println("historical_storage: LittleFS mount failed");
        return false;
    }

    // We don't need to read/write headers anymore. The file size itself
    // dictates the state.
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

    uint32_t now_ms = millis();

    if (!tickInitialized) {
        lastTick_ms = now_ms;
        tickInitialized = true;
        return;
    }

    // Delta-based check handles unsigned integer overflow intrinsically
    if (now_ms - lastTick_ms >= 60000) {
        lastTick_ms += 60000; // Increment exactly 60s to prevent drift
        uptimeMinutes++;

        MinuteRecord rec{};
        rec.uptime_m = uptimeMinutes;
        rec.epoch_s = 0; // Replace with a real RTC/NTP getter if available

        for (uint8_t i = 0; i < kSensorCount; i++) {
            rec.avgPowerW[i] = sampleCount[i] > 0 ? (float)(powerSum[i] / sampleCount[i]) : 0.0f;
            rec.energyWh[i] = (float)energyAccumWh[i];

            // Reset accumulators
            powerSum[i] = 0;
            sampleCount[i] = 0;
            energyAccumWh[i] = 0;
        }

        ramBuffer[ramBufferCount++] = rec;

        if (ramBufferCount >= kRamBufferSize) {
            flushBuffer();
        }
    }
}

size_t getRecent(MinuteRecord* out, size_t maxCount) {
    if (!ready) return 0;

    size_t archCount = getFileRecordCount(kArchiveFile);
    size_t recCount = getFileRecordCount(kRecentFile);
    size_t totalRecords = archCount + recCount + ramBufferCount;

    if (totalRecords == 0) return 0;

    size_t n = (totalRecords < maxCount) ? totalRecords : maxCount;
    size_t startIdx = totalRecords - n; // Absolute index across all three stores
    size_t outIdx = 0;

    File archFile;
    File recFile;

    // Read sequentially from oldest to newest chronologically
    for (size_t i = startIdx; i < totalRecords; i++) {
        if (i < archCount) {
            if (!archFile) archFile = LittleFS.open(kArchiveFile, "r");
            archFile.seek(i * kRecordSize);
            archFile.read((uint8_t*)&out[outIdx++], kRecordSize);
        }
        else if (i < archCount + recCount) {
            if (!recFile) recFile = LittleFS.open(kRecentFile, "r");
            size_t offset = (i - archCount) * kRecordSize;
            recFile.seek(offset);
            recFile.read((uint8_t*)&out[outIdx++], kRecordSize);
        }
        else {
            size_t ramIdx = i - archCount - recCount;
            out[outIdx++] = ramBuffer[ramIdx];
        }
    }

    if (archFile) archFile.close();
    if (recFile) recFile.close();

    return n;
}

size_t getTimeSeries(MinuteRecord* out, size_t maxPoints, uint32_t lookbackMinutes) {
    if (!ready || maxPoints == 0 || lookbackMinutes == 0) return 0;

    size_t archCount = getFileRecordCount(kArchiveFile);
    size_t recCount = getFileRecordCount(kRecentFile);
    size_t totalRecords = archCount + recCount + ramBufferCount;

    if (totalRecords == 0) return 0;

    // Constrain the lookback window to the available dataset
    size_t targetRecords = (lookbackMinutes < totalRecords) ? lookbackMinutes : totalRecords;

    // Calculate the decimation stride.
    // If requesting 10,080 minutes for a 200-point chart, stride = 50.
    // It will read every 50th record, ignoring the intermediate 49.
    size_t stride = (targetRecords > maxPoints) ? (targetRecords / maxPoints) : 1;

    size_t startIdx = totalRecords - targetRecords;
    size_t outIdx = 0;

    File archFile;
    File recFile;

    for (size_t i = startIdx; i < totalRecords && outIdx < maxPoints; i += stride) {
        if (i < archCount) {
            // Data resides in Archive
            if (!archFile) archFile = LittleFS.open(kArchiveFile, "r");
            archFile.seek(i * kRecordSize);
            archFile.read((uint8_t*)&out[outIdx++], kRecordSize);
        }
        else if (i < archCount + recCount) {
            // Data resides in Recent
            if (!recFile) recFile = LittleFS.open(kRecentFile, "r");
            size_t offset = (i - archCount) * kRecordSize;
            recFile.seek(offset);
            recFile.read((uint8_t*)&out[outIdx++], kRecordSize);
        }
        else {
            // Data resides in RAM buffer.
            // If the UI requests lookbackMinutes <= 60, startIdx evaluates here directly.
            // No file handles are opened.
            size_t ramIdx = i - archCount - recCount;
            out[outIdx++] = ramBuffer[ramIdx];
        }
    }

    if (archFile) archFile.close();
    if (recFile) recFile.close();

    return outIdx;
}

size_t recordCount() {
    if (!ready) return 0;
    return getFileRecordCount(kArchiveFile) + getFileRecordCount(kRecentFile) + ramBufferCount;
}

} // namespace historical_storage
