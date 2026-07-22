#include "sensors.h"
#include "sensor_config.h"
#include "sensor_source.h"
#include "sensor_source_adc.h"
#include "sensor_source_ads1115.h"
#include "sensor_source_sim.h"
#include "sensor_source_uart.h"
#include "sensor_mode.h"
#include "sensor_calibration.h"
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

SensorSource* makeSource(SensorId id) {
    if (id >= SENSOR_COUNT) return nullptr;
    switch (sensor_mode::get()) {
        case sensor_mode::Mode::Demo:
            switch (id) {
                case SENSOR_IN: return new SimulatedSensorSource(/*V*/ 18.0f, /*A*/ 2.0f, /*phase*/ 0,
                                                                 /*duty*/ 0.5f, 0.8f);
                case SENSOR_OUT: return new SimulatedSensorSource(/*V*/ 13.0f, /*A*/ 1.5f, /*phase*/ 1);
                case SENSOR_AUX: return new SimulatedSensorSource(/*V*/ 5.0f, /*A*/ 0.4f, /*phase*/ 2);
                default: return nullptr;
            }
        case sensor_mode::Mode::Uart:
            return new UartPm1SensorSource(static_cast<uint8_t>(id));
        case sensor_mode::Mode::Adc: {
            const config::Pins& pins = config::kPins[id];
            return new Esp32AnalogSource(pins.voltage, pins.current);
        }
        case sensor_mode::Mode::Ads1115:
#if POWER_METER_HAS_ADS1115
            switch (id) {
                case SENSOR_IN: return new Ads1115SensorSource(id, 1, 0);
                case SENSOR_OUT: return new Ads1115SensorSource(id, 3, 2);
                case SENSOR_AUX: return new Ads1115SensorSource(id, 0, 0, false);
                default: return nullptr;
            }
#else
            return nullptr;
#endif
    }
    return nullptr;
}
SensorSource* sources[SENSOR_COUNT] = {nullptr, nullptr, nullptr};
bool sourceReady[SENSOR_COUNT] = {false, false, false};
uint32_t lastSourceInitAttemptMs[SENSOR_COUNT] = {0, 0, 0};
constexpr uint32_t kSourceInitRetryMs = 5000;

Reading makeReading(uint32_t now, SensorSample sample, bool applyCalibration, uint8_t sensor) {
    Reading reading;
    reading.timestamp_ms = now;
    reading.configured = sample.configured;
    reading.state = ReadingState::Waiting;

    switch (sample.state) {
        case SensorSampleState::NotConfigured:
            reading.configured = false;
            reading.state = ReadingState::NotConfigured;
            return reading;
        case SensorSampleState::Waiting:
            return reading;
        case SensorSampleState::Invalid:
            reading.state = ReadingState::Invalid;
            return reading;
        case SensorSampleState::Stale:
            reading.state = ReadingState::Stale;
            return reading;
        case SensorSampleState::Observed:
            break;
    }

    reading.voltageInputV = sample.voltage;
    reading.currentInputV = sample.current;
    reading.voltage = sample.voltage;
    reading.current = sample.current;
    if (applyCalibration) {
        const calibration::Source calibrationSource =
            sensor_mode::get() == sensor_mode::Mode::Ads1115
                ? calibration::Source::Ads1115 : calibration::Source::Esp32Adc;
        reading.voltage = calibration::apply(
            sample.voltage, calibration::get(calibrationSource, sensor, calibration::Measurement::Voltage));
        reading.current = calibration::apply(
            sample.current, calibration::get(calibrationSource, sensor, calibration::Measurement::Current));
    }

    if (sample.hasDutyCycle) {
        reading.dutyCycle = sample.dutyCycle;
        reading.dutyState = isPlausibleDutyCycle(sample.dutyCycle) ? DutyState::Valid : DutyState::Invalid;
    }

    const bool finiteVoltage = std::isfinite(reading.voltage);
    const bool finiteCurrent = std::isfinite(reading.current);
    if (finiteVoltage && finiteCurrent) {
        reading.power = reading.voltage * reading.current;
    } else {
        reading.power = NAN;
    }

    if (!finiteVoltage || !finiteCurrent || !std::isfinite(reading.power)) {
        reading.state = ReadingState::Invalid;
    } else if (!isPlausibleVoltage(reading.voltage) || !isPlausibleCurrent(reading.current)) {
        reading.state = ReadingState::OutOfRange;
    } else {
        reading.state = ReadingState::Valid;
    }
    return reading;
}

void taskFn(void*) {
    TickType_t lastWake = xTaskGetTickCount();
    for (;;) {
        uint32_t now = millis();
        Reading readings[SENSOR_COUNT];
        for (uint8_t i = 0; i < SENSOR_COUNT; i++) {
            SensorSample sample;
            if (sources[i] && !sourceReady[i] &&
                now - lastSourceInitAttemptMs[i] >= kSourceInitRetryMs) {
                lastSourceInitAttemptMs[i] = now;
                sourceReady[i] = sources[i]->init();
                if (sourceReady[i]) Serial.printf("sensors: sensor %u recovered\n", i);
            }
            if (!sources[i] || !sourceReady[i]) {
                sample.state = SensorSampleState::Invalid;
                sample.configured = sensor_mode::get() != sensor_mode::Mode::Uart;
            }
            else sample = sources[i]->read();
            readings[i] = makeReading(now, sample,
                                      sources[i] && sources[i]->requiresCalibration(), i);
        }
        // Physical I/O and calibration happen outside the history lock. API,
        // display, and storage readers are blocked only for this short copy.
        xSemaphoreTake(mutex, portMAX_DELAY);
        for (uint8_t i = 0; i < SENSOR_COUNT; i++) {
            buffer[i][writeIndex[i]] = readings[i];
            writeIndex[i] = (writeIndex[i] + 1) % kHistorySize;
            if (count[i] < kHistorySize) count[i]++;
        }
        xSemaphoreGive(mutex);
        vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(kSampleIntervalMs));
    }
}

} // namespace

void start() {
    calibration::init();
    mutex = xSemaphoreCreateMutex();
    if (!mutex) {
        Serial.println("sensors: failed to create mutex");
        return;
    }

    randomSeed(esp_random());
    for (uint8_t i = 0; i < SENSOR_COUNT; i++) {
        sources[i] = makeSource(static_cast<SensorId>(i));
        lastSourceInitAttemptMs[i] = millis();
        sourceReady[i] = sources[i] && sources[i]->init();
        if (!sourceReady[i]) {
            Serial.printf("sensors: sensor %u failed to init\n", i);
        }
    }
    if (xTaskCreatePinnedToCore(taskFn, "sensors_task", 4096, nullptr, 1, nullptr, 0) != pdPASS) {
        Serial.println("sensors: failed to start task");
    }
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

bool isConfigured(const Reading& reading) {
    return reading.configured;
}

bool isCalculationEligible(const Reading& reading) {
    return reading.state == ReadingState::Valid;
}

bool isDirectDutyEligible(const Reading& reading) {
    return isCalculationEligible(reading) && reading.dutyState == DutyState::Valid;
}

uint8_t getConfiguredMask() {
    if (!mutex) return 0;
    uint8_t mask = 0;
    xSemaphoreTake(mutex, portMAX_DELAY);
    for (uint8_t i = 0; i < SENSOR_COUNT; ++i) {
        if (count[i] > 0) {
            const Reading& reading = buffer[i][(writeIndex[i] + kHistorySize - 1) % kHistorySize];
            if (isConfigured(reading)) mask |= static_cast<uint8_t>(1U << i);
        }
    }
    xSemaphoreGive(mutex);
    return mask;
}

float getDutyCycle(SensorId id, size_t window) {
    if (!mutex || id >= SENSOR_COUNT) return 1.0f;
    if (window > kDutyWindowSize) window = kDutyWindowSize; // clamp to stack buffer size

    // Snapshot the window's power values under the lock, then do the math
    // (sorting etc.) outside it so we hold the mutex as briefly as possible.
    float powers[kDutyWindowSize];
    float directDuties[kDutyWindowSize];
    size_t n;
    DutyState latestDutyState = DutyState::NotReported;
    xSemaphoreTake(mutex, portMAX_DELAY);
    n = count[id] < window ? count[id] : window;
    size_t start = (writeIndex[id] + kHistorySize - n) % kHistorySize;
    for (size_t i = 0; i < n; i++) {
        const Reading& reading = buffer[id][(start + i) % kHistorySize];
        powers[i] = isCalculationEligible(reading) ? reading.power : NAN;
        directDuties[i] = isDirectDutyEligible(reading) ? reading.dutyCycle : NAN;
    }
    if (n) latestDutyState = buffer[id][(writeIndex[id] + kHistorySize - 1) % kHistorySize].dutyState;
    xSemaphoreGive(mutex);

    if (n == 0) return 1.0f;

    // A source that explicitly reported an invalid duty must remain
    // unavailable. Falling back to inference would hide the data-quality
    // failure and could manufacture available-power history.
    if (latestDutyState == DutyState::Invalid) return NAN;

    // A source that samples faster than our 500 ms history cadence can
    // provide the true PWM duty while voltage/current carry its averaged
    // effect. Average the most recent second for a stable KPI.
    if (std::isfinite(directDuties[n - 1])) {
        const size_t requestedCount = std::min(n, static_cast<size_t>(1000 / kSampleIntervalMs));
        float dutySum = 0.0f;
        size_t directCount = 0;
        for (size_t i = n - requestedCount; i < n; ++i) {
            if (!std::isfinite(directDuties[i])) continue;
            dutySum += directDuties[i];
            ++directCount;
        }
        if (directCount == 0) return 1.0f;
        return std::max(0.0f, std::min(1.0f, dutySum / directCount));
    }

    float sum = 0;
    size_t eligibleCount = 0;
    for (size_t i = 0; i < n; i++) {
        if (!std::isfinite(powers[i])) continue;
        powers[eligibleCount++] = powers[i];
        sum += powers[i];
    }
    if (eligibleCount == 0) return 1.0f;
    float mean = sum / eligibleCount;

    if (std::fabs(mean) < kMinPowerForDutyWatts) return 1.0f;

    // "Near-peak" = a value close to the top of the window, skipping a
    // couple of the very highest samples so a single noise spike doesn't
    // read as "100% available". How many we skip scales with window size
    // (~2%) so a small window like the default 60 doesn't throw away its
    // only good sample.
    std::sort(powers, powers + eligibleCount);
    size_t skip = eligibleCount / 50;
    if (skip >= eligibleCount) skip = eligibleCount - 1;
    float peak = powers[eligibleCount - 1 - skip];

    if (std::fabs(peak) < kMinPowerForDutyWatts) return 1.0f;

    float duty = mean / peak;
    if (duty < 0.0f) duty = 0.0f;
    if (duty > 1.0f) duty = 1.0f;
    return duty;
}

bool getAvailablePower(SensorId id, float& outWatts) {
    Reading latest;
    if (!getLatest(id, latest) || !isCalculationEligible(latest)) return false;
    float duty = getDutyCycle(id);
    if (!std::isfinite(duty)) return false;
    if (duty < 0.01f) {
        outWatts = latest.power; // avoid dividing by ~0
        return true;
    }
    outWatts = latest.power / duty;
    return true;
}

bool getNetBatteryPower(float& outWatts) {
    Reading in, out;
    if (!getLatest(SENSOR_IN, in) || !getLatest(SENSOR_OUT, out) ||
        !isCalculationEligible(in) || !isCalculationEligible(out)) return false;
    outWatts = in.power - out.power;
    return true;
}

} // namespace sensors
