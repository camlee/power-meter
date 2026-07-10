#include "sensors.h"
#include "sensor_source.h"
#include "sensor_source_sim.h"
#include <algorithm>
#include <cmath>
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

namespace sensors {
namespace {

// --- Ring buffer state, one set per sensor -------------------------------
Reading buffer[SENSOR_COUNT][kHistorySize];
size_t writeIndex[SENSOR_COUNT] = {0, 0, 0};
size_t count[SENSOR_COUNT] = {0, 0, 0};
SemaphoreHandle_t mutex = nullptr; // one mutex guards all 3 buffers; they're small

// Below this, mean/peak power is too small to say anything meaningful about
// duty cycle, so we just report 1.0 (fully on) instead of a noisy ratio.
constexpr float kMinPowerForDutyWatts = 0.5f;

// --- Sensor sources -------------------------------------------------------
// THIS IS THE ONLY PLACE THAT NEEDS TO CHANGE once real hardware is known.
// Replace each `new SimulatedSensorSource(...)` with e.g.
//   new Ina219SensorSource(I2C_ADDR_0)
// as long as the replacement implements SensorSource, nothing else in the
// app (UI, storage) needs to be touched.
SensorSource* makeSource(SensorId id) {
    switch (id) {
        case SENSOR_IN:  return new SimulatedSensorSource(/*V*/ 18.0f, /*A*/ 2.0f, /*phase*/ 0);
        case SENSOR_OUT: return new SimulatedSensorSource(/*V*/ 13.0f, /*A*/ 1.5f, /*phase*/ 1);
        case SENSOR_AUX: return new SimulatedSensorSource(/*V*/ 5.0f, /*A*/ 0.4f, /*phase*/ 2);
        default: return nullptr;
    }
}
SensorSource* sources[SENSOR_COUNT] = {nullptr, nullptr, nullptr};

void taskFn(void*) {
    TickType_t lastWake = xTaskGetTickCount();
    for (;;) {
        uint32_t now = millis();
        xSemaphoreTake(mutex, portMAX_DELAY);
        for (uint8_t i = 0; i < SENSOR_COUNT; i++) {
            SensorSample s = sources[i]->read();
            Reading r{now, s.voltage, s.current, s.voltage * s.current};
            buffer[i][writeIndex[i]] = r;
            writeIndex[i] = (writeIndex[i] + 1) % kHistorySize;
            if (count[i] < kHistorySize) count[i]++;
        }
        xSemaphoreGive(mutex);
        vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(kSampleIntervalMs));
    }
}

} // namespace

void start() {
    mutex = xSemaphoreCreateMutex();
    randomSeed(esp_random());
    for (uint8_t i = 0; i < SENSOR_COUNT; i++) {
        sources[i] = makeSource(static_cast<SensorId>(i));
        if (!sources[i]->init()) {
            Serial.printf("sensors: sensor %u failed to init\n", i);
        }
    }
    xTaskCreatePinnedToCore(taskFn, "sensors_task", 4096, nullptr, 1, nullptr, 0);
}

size_t getRecent(SensorId id, Reading* out, size_t maxCount) {
    if (!mutex || id >= SENSOR_COUNT) return 0;
    xSemaphoreTake(mutex, portMAX_DELAY);
    size_t n = count[id] < maxCount ? count[id] : maxCount;
    size_t start = (writeIndex[id] + kHistorySize - n) % kHistorySize;
    for (size_t i = 0; i < n; i++) {
        out[i] = buffer[id][(start + i) % kHistorySize];
    }
    xSemaphoreGive(mutex);
    return n;
}

bool getLatest(SensorId id, Reading& out) {
    if (!mutex || id >= SENSOR_COUNT || count[id] == 0) return false;
    xSemaphoreTake(mutex, portMAX_DELAY);
    out = buffer[id][(writeIndex[id] + kHistorySize - 1) % kHistorySize];
    xSemaphoreGive(mutex);
    return true;
}

float getDutyCycle(SensorId id, size_t window) {
    if (!mutex || id >= SENSOR_COUNT) return 1.0f;
    if (window > kDutyWindowSize) window = kDutyWindowSize; // clamp to stack buffer size

    // Snapshot the window's power values under the lock, then do the math
    // (sorting etc.) outside it so we hold the mutex as briefly as possible.
    float powers[kDutyWindowSize];
    size_t n;
    xSemaphoreTake(mutex, portMAX_DELAY);
    n = count[id] < window ? count[id] : window;
    size_t start = (writeIndex[id] + kHistorySize - n) % kHistorySize;
    for (size_t i = 0; i < n; i++) {
        powers[i] = buffer[id][(start + i) % kHistorySize].power;
    }
    xSemaphoreGive(mutex);

    if (n == 0) return 1.0f;

    float sum = 0;
    for (size_t i = 0; i < n; i++) sum += powers[i];
    float mean = sum / n;

    if (std::fabs(mean) < kMinPowerForDutyWatts) return 1.0f;

    // "Near-peak" = a value close to the top of the window, skipping a
    // couple of the very highest samples so a single noise spike doesn't
    // read as "100% available". How many we skip scales with window size
    // (~2%) so a small window like the default 60 doesn't throw away its
    // only good sample.
    std::sort(powers, powers + n);
    size_t skip = n / 50;
    if (skip >= n) skip = n - 1;
    float peak = powers[n - 1 - skip];

    if (std::fabs(peak) < kMinPowerForDutyWatts) return 1.0f;

    float duty = mean / peak;
    if (duty < 0.0f) duty = 0.0f;
    if (duty > 1.0f) duty = 1.0f;
    return duty;
}

bool getAvailablePower(SensorId id, float& outWatts) {
    Reading latest;
    if (!getLatest(id, latest)) return false;
    float duty = getDutyCycle(id);
    if (duty < 0.01f) {
        outWatts = latest.power; // avoid dividing by ~0
        return true;
    }
    outWatts = latest.power / duty;
    return true;
}

bool getNetBatteryPower(float& outWatts) {
    Reading in, out;
    if (!getLatest(SENSOR_IN, in) || !getLatest(SENSOR_OUT, out)) return false;
    outWatts = in.power - out.power;
    return true;
}

} // namespace sensors
