#include "historical_storage.h"
#include <Arduino.h>
#include <LittleFS.h>
#include "demo_history_profile.h"
#include "../sensors/sensor_mode.h"

namespace historical_storage {
namespace {

// New names intentionally start this early-development data model fresh.
// The old raw-only history cannot faithfully reconstruct charge/discharge
// transitions after it has already been summed into minute records.
const char* historyRecentPath() { return sensor_mode::get() == sensor_mode::Mode::Demo ? "/demo_history_A.bin" : "/real_history_A.bin"; }
const char* historyArchivePath() { return sensor_mode::get() == sensor_mode::Mode::Demo ? "/demo_history_B.bin" : "/real_history_B.bin"; }
const char* recentFile() { return historyRecentPath(); }
const char* archiveFile() { return historyArchivePath(); }

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

double energyAccumWh[kSensorCount] = {0, 0, 0};
double componentEnergyAccumWh[COMPONENT_COUNT] = {0, 0, 0, 0, 0};
uint32_t lastFrameTime_ms = 0;
bool haveLastFrameTime = false;


// --- Internal Helpers ----------------------------------------------------

size_t getFileRecordCount(const char* path) {
    File f = LittleFS.open(path, "r");
    if (!f) return 0;
    size_t sz = f.size();
    f.close();
    return sz / kRecordSize;
}

bool readRecordAt(size_t absoluteIndex, size_t archiveCount, size_t recentCount,
                  File& archiveFile, File& recentFile, MinuteRecord& out) {
    if (absoluteIndex < archiveCount) {
        if (!archiveFile) archiveFile = LittleFS.open(historyArchivePath(), "r");
        if (!archiveFile) return false;
        archiveFile.seek(absoluteIndex * kRecordSize);
        return archiveFile.read(reinterpret_cast<uint8_t*>(&out), kRecordSize) == kRecordSize;
    }
    if (absoluteIndex < archiveCount + recentCount) {
        if (!recentFile) recentFile = LittleFS.open(historyRecentPath(), "r");
        if (!recentFile) return false;
        recentFile.seek((absoluteIndex - archiveCount) * kRecordSize);
        return recentFile.read(reinterpret_cast<uint8_t*>(&out), kRecordSize) == kRecordSize;
    }

    const size_t ramIndex = absoluteIndex - archiveCount - recentCount;
    if (ramIndex >= ramBufferCount) return false;
    out = ramBuffer[ramIndex];
    return true;
}

void rotateFiles() {
    // Drop the oldest half of the data, and promote recent to archive
    if (LittleFS.exists(archiveFile())) {
        LittleFS.remove(archiveFile());
    }
    LittleFS.rename(recentFile(), archiveFile());
}

void flushBuffer() {
    if (ramBufferCount == 0) return;

    File f = LittleFS.open(recentFile(), "a");
    if (f) {
        // Bulk write the entire array of structs
        f.write((const uint8_t*)ramBuffer, ramBufferCount * kRecordSize);
        f.close();
    }

    ramBufferCount = 0;

    // Check if it's time to rotate
    if (getFileRecordCount(recentFile()) >= kMaxRecordsPerFile) {
        rotateFiles();
    }
}

void seedDemoHistory() {
    if (sensor_mode::get() != sensor_mode::Mode::Demo || LittleFS.exists(recentFile())) return;
    File file = LittleFS.open(recentFile(), "w");
    if (!file) return;
    for (uint32_t minute = 1; minute <= 20160; ++minute) {
        const auto& point = demo::kDayProfile[((minute - 1) / 15) % 96];
        const float charge = point.chargeW10 / 10.0f;
        const float use = point.useW10 / 10.0f;
        const float panel = point.panelW10 / 10.0f;
        const float surplus = point.surplusW10 / 10.0f;
        MinuteRecord record{};
        record.uptime_m = minute;
        record.energyWh[0] = (charge + panel) / 60.0f;
        record.energyWh[1] = (use + panel) / 60.0f;
        record.componentEnergyWh[BATTERY_CHARGING] = charge / 60.0f;
        record.componentEnergyWh[BATTERY_USAGE] = use / 60.0f;
        record.componentEnergyWh[PANEL_IN] = panel / 60.0f;
        record.componentEnergyWh[PANEL_USAGE] = panel / 60.0f;
        record.componentEnergyWh[PANEL_SURPLUS] = surplus / 60.0f;
        file.write(reinterpret_cast<const uint8_t*>(&record), sizeof(record));
    }
    file.close();
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
    seedDemoHistory();
    uptimeMinutes = getFileRecordCount(archiveFile()) + getFileRecordCount(recentFile());
    return true;
}

void addSampleFrame(float inPowerW, float outPowerW, float auxPowerW,
                    float availableInPowerW, uint32_t timestamp_ms) {
    if (!ready) return;

    if (haveLastFrameTime) {
        const uint32_t dt_ms = timestamp_ms - lastFrameTime_ms;
        const double hours = static_cast<double>(dt_ms) / 3600000.0;
        const float in = inPowerW > 0.0f ? inPowerW : 0.0f;
        const float out = outPowerW > 0.0f ? outPowerW : 0.0f;
        const float aux = auxPowerW > 0.0f ? auxPowerW : 0.0f;
        const float netBattery = in - out;
        const float panelToLoad = in < out ? in : out;

        energyAccumWh[0] += in * hours;
        energyAccumWh[1] += out * hours;
        energyAccumWh[2] += aux * hours;
        componentEnergyAccumWh[BATTERY_CHARGING] += (netBattery > 0.0f ? netBattery : 0.0f) * hours;
        componentEnergyAccumWh[BATTERY_USAGE] += (netBattery < 0.0f ? -netBattery : 0.0f) * hours;
        componentEnergyAccumWh[PANEL_IN] += panelToLoad * hours;
        componentEnergyAccumWh[PANEL_USAGE] += panelToLoad * hours;
        componentEnergyAccumWh[PANEL_SURPLUS] +=
            (availableInPowerW > in ? availableInPowerW - in : 0.0f) * hours;
    }
    lastFrameTime_ms = timestamp_ms;
    haveLastFrameTime = true;
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
            rec.energyWh[i] = (float)energyAccumWh[i];
            energyAccumWh[i] = 0;
        }
        for (uint8_t i = 0; i < COMPONENT_COUNT; ++i) {
            rec.componentEnergyWh[i] = static_cast<float>(componentEnergyAccumWh[i]);
            componentEnergyAccumWh[i] = 0;
        }

        ramBuffer[ramBufferCount++] = rec;

        if (ramBufferCount >= kRamBufferSize) {
            flushBuffer();
        }
    }
}

size_t getRecent(MinuteRecord* out, size_t maxCount) {
    if (!ready) return 0;

    size_t archCount = getFileRecordCount(archiveFile());
    size_t recCount = getFileRecordCount(recentFile());
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
            if (!archFile) archFile = LittleFS.open(archiveFile(), "r");
            archFile.seek(i * kRecordSize);
            archFile.read((uint8_t*)&out[outIdx++], kRecordSize);
        }
        else if (i < archCount + recCount) {
            if (!recFile) recFile = LittleFS.open(recentFile(), "r");
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

    size_t archCount = getFileRecordCount(archiveFile());
    size_t recCount = getFileRecordCount(recentFile());
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
            if (!archFile) archFile = LittleFS.open(archiveFile(), "r");
            archFile.seek(i * kRecordSize);
            archFile.read((uint8_t*)&out[outIdx++], kRecordSize);
        }
        else if (i < archCount + recCount) {
            // Data resides in Recent
            if (!recFile) recFile = LittleFS.open(recentFile(), "r");
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

size_t getPowerBuckets(PowerBucket* out, size_t maxBuckets,
                       uint32_t lookbackMinutes, uint16_t bucketMinutes,
                       uint32_t endOffsetMinutes, bool includePartial) {
    if (!ready || !out || maxBuckets == 0 || lookbackMinutes == 0 || bucketMinutes == 0) return 0;

    const size_t archiveCount = getFileRecordCount(archiveFile());
    const size_t recentCount = getFileRecordCount(recentFile());
    const size_t totalRecords = archiveCount + recentCount + ramBufferCount;
    if (totalRecords == 0) return 0;

    // Include one preceding bucket so a relative window ending in the middle
    // of a bucket can still return all of its completed buckets.
    const uint32_t requestedMinutes = lookbackMinutes + endOffsetMinutes + bucketMinutes;
    const size_t recordsToRead = requestedMinutes < totalRecords ? requestedMinutes : totalRecords;
    const size_t firstRecord = totalRecords - recordsToRead;

    File archiveFile;
    File recentFile;
    MinuteRecord record{};
    uint32_t previousUptime = 0;
    uint32_t activeStart = 0;
    uint16_t activeCount = 0;
    double activeEnergy[kSensorCount] = {0, 0, 0};
    double activeComponentEnergy[COMPONENT_COUNT] = {0, 0, 0, 0, 0};
    bool havePrevious = false;
    size_t bucketCount = 0;

    for (size_t i = firstRecord; i < totalRecords; ++i) {
        if (!readRecordAt(i, archiveCount, recentCount, archiveFile, recentFile, record)) break;

        const uint32_t bucketStart = ((record.uptime_m - 1) / bucketMinutes) * bucketMinutes + 1;
        const bool continuesBucket = havePrevious && record.uptime_m == previousUptime + 1 &&
                                     bucketStart == activeStart;
        if (!continuesBucket) {
            activeStart = bucketStart;
            activeCount = 0;
            for (uint8_t channel = 0; channel < kSensorCount; ++channel) activeEnergy[channel] = 0;
            for (uint8_t component = 0; component < COMPONENT_COUNT; ++component) activeComponentEnergy[component] = 0;
        }

        ++activeCount;
        for (uint8_t channel = 0; channel < kSensorCount; ++channel) {
            activeEnergy[channel] += record.energyWh[channel];
        }
        for (uint8_t component = 0; component < COMPONENT_COUNT; ++component) {
            activeComponentEnergy[component] += record.componentEnergyWh[component];
        }

        if (activeCount == bucketMinutes) {
            // Retain the newest results when the caller has a smaller buffer.
            if (bucketCount == maxBuckets) {
                memmove(out, out + 1, (maxBuckets - 1) * sizeof(PowerBucket));
                --bucketCount;
            }
            PowerBucket& bucket = out[bucketCount++];
            bucket.startUptime_m = activeStart;
            bucket.durationMinutes = bucketMinutes;
            for (uint8_t channel = 0; channel < kSensorCount; ++channel) {
                bucket.energyWh[channel] = static_cast<float>(activeEnergy[channel]);
            }
            for (uint8_t component = 0; component < COMPONENT_COUNT; ++component) {
                bucket.componentEnergyWh[component] = static_cast<float>(activeComponentEnergy[component]);
                bucket.componentAveragePowerW[component] = bucket.componentEnergyWh[component] * 60.0f / bucket.durationMinutes;
            }
        }

        previousUptime = record.uptime_m;
        havePrevious = true;
    }

    if (archiveFile) archiveFile.close();
    if (recentFile) recentFile.close();

    if (includePartial && activeCount > 0 && activeCount < bucketMinutes && bucketCount < maxBuckets) {
        PowerBucket& bucket = out[bucketCount++];
        bucket.startUptime_m = activeStart;
        bucket.durationMinutes = activeCount;
        for (uint8_t channel = 0; channel < kSensorCount; ++channel) bucket.energyWh[channel] = static_cast<float>(activeEnergy[channel]);
        for (uint8_t component = 0; component < COMPONENT_COUNT; ++component) {
            bucket.componentEnergyWh[component] = static_cast<float>(activeComponentEnergy[component]);
            bucket.componentAveragePowerW[component] = bucket.componentEnergyWh[component] * 60.0f / activeCount;
        }
    }

    const size_t newerBuckets = endOffsetMinutes / bucketMinutes;
    const size_t wantedBuckets = (lookbackMinutes + bucketMinutes - 1) / bucketMinutes;
    if (bucketCount <= newerBuckets) return 0;
    const size_t available = bucketCount - newerBuckets;
    const size_t returned = available < wantedBuckets ? available : wantedBuckets;
    const size_t start = bucketCount - newerBuckets - returned;
    if (start > 0) memmove(out, out + start, returned * sizeof(PowerBucket));
    return returned;
}

size_t recordCount() {
    if (!ready) return 0;
    return getFileRecordCount(archiveFile()) + getFileRecordCount(recentFile()) + ramBufferCount;
}

} // namespace historical_storage
