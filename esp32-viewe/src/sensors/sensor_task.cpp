#include "sensor_task.h"
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

namespace sensor_task {
namespace {

Reading buffer[kHistorySize];
size_t writeIndex = 0;
size_t count = 0;
SemaphoreHandle_t mutex = nullptr;

constexpr uint32_t kSampleIntervalMs = 500;
constexpr float kStepSize = 2.0f;
constexpr float kMin = 0.0f;
constexpr float kMax = 100.0f;

void taskFn(void*) {
    float value = (kMin + kMax) / 2.0f;

    for (;;) {
        float step = ((float)random(-1000, 1001) / 1000.0f) * kStepSize;
        value += step;
        // reflect off the bounds so it doesn't just clamp and flatline
        if (value < kMin) value = kMin + (kMin - value);
        if (value > kMax) value = kMax - (value - kMax);

        Reading r{value, millis()};

        xSemaphoreTake(mutex, portMAX_DELAY);
        buffer[writeIndex] = r;
        writeIndex = (writeIndex + 1) % kHistorySize;
        if (count < kHistorySize) count++;
        xSemaphoreGive(mutex);

        vTaskDelay(pdMS_TO_TICKS(kSampleIntervalMs));
    }
}

} // namespace

void start() {
    mutex = xSemaphoreCreateMutex();
    randomSeed(esp_random());
    // Pinned to core 0; LVGL's own port task typically runs on core 1 (check
    // lvgl_v8_port.cpp) — keep this off whichever core is doing the rendering.
    xTaskCreatePinnedToCore(taskFn, "sensor_task", 4096, nullptr, 1, nullptr, 0);
}

size_t getRecent(Reading* out, size_t maxCount) {
    if (!mutex) return 0;
    xSemaphoreTake(mutex, portMAX_DELAY);
    size_t n = count < maxCount ? count : maxCount;
    size_t start = (writeIndex + kHistorySize - n) % kHistorySize;
    for (size_t i = 0; i < n; i++) {
        out[i] = buffer[(start + i) % kHistorySize];
    }
    xSemaphoreGive(mutex);
    return n;
}

bool getLatest(Reading& out) {
    if (!mutex || count == 0) return false;
    xSemaphoreTake(mutex, portMAX_DELAY);
    out = buffer[(writeIndex + kHistorySize - 1) % kHistorySize];
    xSemaphoreGive(mutex);
    return true;
}

} // namespace sensor_task
