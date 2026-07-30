#pragma once

#include "sensors.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace sensors {

// Quality metadata for one high-rate acquisition window. A window remains
// operational when at least 80% of its samples are valid; rejected spikes are
// excluded from its mean instead of invalidating or contaminating the entire
// 500 ms reading.
struct AdcWindowQuality {
    uint16_t sampleCount = 0;
    uint16_t validSampleCount = 0;
    uint16_t rejectedSampleCount = 0;
    bool toleratedRejections = false;
};

class AdcWindowReducer {
public:
    void add(const Reading& reading) {
        ++sampleCount_;
        configured_ |= reading.configured;

        const bool observed =
            (reading.state == ReadingState::Valid ||
             reading.state == ReadingState::OutOfRange) &&
            std::isfinite(reading.voltage) &&
            std::isfinite(reading.current) &&
            std::isfinite(reading.power);
        if (observed) {
            ++observedCount_;
            observedVoltageInputSum_ += reading.voltageInputV;
            observedCurrentInputSum_ += reading.currentInputV;
            observedVoltageSum_ += reading.voltage;
            observedCurrentSum_ += reading.current;
            observedPowerSum_ += reading.power;
        }

        if (reading.state == ReadingState::Valid && observed) {
            ++validCount_;
            validVoltageInputSum_ += reading.voltageInputV;
            validCurrentInputSum_ += reading.currentInputV;
            validVoltageSum_ += reading.voltage;
            validCurrentSum_ += reading.current;
            validPowerSum_ += reading.power;
            const float magnitude = std::fabs(reading.power);
            absolutePowerSum_ += magnitude;
            if (magnitude >= highestAbsolutePower_) {
                secondHighestAbsolutePower_ = highestAbsolutePower_;
                highestAbsolutePower_ = magnitude;
            } else if (magnitude > secondHighestAbsolutePower_) {
                secondHighestAbsolutePower_ = magnitude;
            }
            return;
        }

        if (reading.state == ReadingState::OutOfRange) {
            rejectedState_ = ReadingState::OutOfRange;
        } else if (rejectedState_ != ReadingState::OutOfRange) {
            rejectedState_ = reading.state;
        }
    }

    Reading finish(uint32_t now, AdcWindowQuality* quality = nullptr) const {
        const uint16_t rejectedCount =
            sampleCount_ >= validCount_ ? sampleCount_ - validCount_ : 0;
        const bool sufficientlyValid =
            validCount_ > 0 &&
            static_cast<uint32_t>(validCount_) * 5U >=
                static_cast<uint32_t>(sampleCount_) * 4U;
        if (quality) {
            quality->sampleCount = sampleCount_;
            quality->validSampleCount = validCount_;
            quality->rejectedSampleCount = rejectedCount;
            quality->toleratedRejections =
                sufficientlyValid && rejectedCount > 0;
        }

        Reading result;
        result.timestamp_ms = now;
        result.configured = configured_;
        if (!configured_) {
            result.state = ReadingState::NotConfigured;
            return result;
        }
        if (sufficientlyValid) {
            assignMean(
                result, validCount_, validVoltageInputSum_,
                validCurrentInputSum_, validVoltageSum_, validCurrentSum_,
                validPowerSum_);
            result.state = ReadingState::Valid;
            assignDuty(result);
            return result;
        }
        if (observedCount_ > 0) {
            // Preserve the observed engineering values for calibration and
            // diagnostics, while keeping the window ineligible for system
            // calculations.
            assignMean(
                result, observedCount_, observedVoltageInputSum_,
                observedCurrentInputSum_, observedVoltageSum_,
                observedCurrentSum_, observedPowerSum_);
            result.state = rejectedState_;
            return result;
        }
        result.state =
            sampleCount_ > 0 ? rejectedState_ : ReadingState::Waiting;
        return result;
    }

    void reset() { *this = AdcWindowReducer{}; }

private:
    static constexpr float kMinPowerForDutyWatts = 0.5f;

    static void assignMean(
        Reading& result, uint16_t count, float voltageInputSum,
        float currentInputSum, float voltageSum, float currentSum,
        float powerSum) {
        const float divisor = static_cast<float>(count);
        result.voltageInputV = voltageInputSum / divisor;
        result.currentInputV = currentInputSum / divisor;
        result.voltage = voltageSum / divisor;
        result.current = currentSum / divisor;
        // Average instantaneous products rather than multiplying independent
        // means; PWM can correlate voltage and current.
        result.power = powerSum / divisor;
    }

    void assignDuty(Reading& result) const {
        const float meanMagnitude =
            absolutePowerSum_ / static_cast<float>(validCount_);
        const float nearPeak = validCount_ > 1
            ? secondHighestAbsolutePower_ : highestAbsolutePower_;
        result.dutyState = DutyState::Valid;
        if (meanMagnitude < kMinPowerForDutyWatts ||
            nearPeak < kMinPowerForDutyWatts) {
            result.dutyCycle = 1.0f;
        } else {
            result.dutyCycle = std::max(
                0.0f, std::min(1.0f, meanMagnitude / nearPeak));
        }
    }

    uint16_t sampleCount_ = 0;
    uint16_t validCount_ = 0;
    uint16_t observedCount_ = 0;
    bool configured_ = false;
    ReadingState rejectedState_ = ReadingState::Invalid;

    float validVoltageInputSum_ = 0.0f;
    float validCurrentInputSum_ = 0.0f;
    float validVoltageSum_ = 0.0f;
    float validCurrentSum_ = 0.0f;
    float validPowerSum_ = 0.0f;

    float observedVoltageInputSum_ = 0.0f;
    float observedCurrentInputSum_ = 0.0f;
    float observedVoltageSum_ = 0.0f;
    float observedCurrentSum_ = 0.0f;
    float observedPowerSum_ = 0.0f;

    float absolutePowerSum_ = 0.0f;
    float highestAbsolutePower_ = 0.0f;
    float secondHighestAbsolutePower_ = 0.0f;
};

} // namespace sensors
