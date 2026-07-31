#pragma once

#include "sensor_limits.h"
#include <algorithm>
#include <cmath>
#include <stdint.h>

namespace sensors::calibration {

enum class Measurement : uint8_t { Voltage = 0, Current = 1 };
enum class Source : uint8_t { Esp32Adc = 0, Ads1115 = 1, Count };

// Engineering value = (ADC input volts - offsetInputV) * gain.
// Keeping the offset in ADC volts matches the electrical behaviour of the
// divider/current-sensor circuitry and the original hard-coded conversion.
struct Value {
    float gain;
    float offsetInputV;
};

constexpr float kAdcMinInputV = 0.0f;
constexpr float kAdcMaxInputV = 3.3f;
// Stored profiles get a broad corruption guard. User-entered candidates are
// constrained by their implied full-scale engineering output instead.
constexpr float kMaximumStoredGainPerInputVolt = 10000.0f;
constexpr float kVoltageMaxV = kMaximumVoltageV;
constexpr float kCurrentMaxA = kMaximumCurrentA;
constexpr float kMinimumGainCalculationDenominatorV = 0.005f;

struct OutputRange {
    float minimum;
    float maximum;
};

enum class ValidationPolicy : uint8_t {
    StoredProfile,
    CommitCandidate,
};

enum class ValidationIssue : uint8_t {
    None,
    GainNotFinite,
    GainNotPositive,
    GainExceedsStorageLimit,
    OffsetNotFinite,
    OffsetBelowAdcRange,
    OffsetAboveAdcRange,
    OutputNotFinite,
    OutputBelowSanityLimit,
    OutputAboveSanityLimit,
};

struct ValidationResult {
    ValidationIssue issue = ValidationIssue::None;
    OutputRange outputRange{};
    float adcInputV = NAN;
    float impliedOutput = NAN;
    float limit = NAN;

    bool accepted() const { return issue == ValidationIssue::None; }
};

// Evaluate the complete range a calibration can produce for any valid ADC
// input. Sanity checks use this stable range, never the incidental live sample.
inline OutputRange fullAdcOutputRange(Value value) {
    const float atMinimumInput =
        (kAdcMinInputV - value.offsetInputV) * value.gain;
    const float atMaximumInput =
        (kAdcMaxInputV - value.offsetInputV) * value.gain;
    return {
        std::min(atMinimumInput, atMaximumInput),
        std::max(atMinimumInput, atMaximumInput),
    };
}

inline ValidationResult validate(
    Measurement measurement, Value value,
    ValidationPolicy policy = ValidationPolicy::CommitCandidate) {
    ValidationResult result{};
    if (!std::isfinite(value.gain)) {
        result.issue = ValidationIssue::GainNotFinite;
        return result;
    }
    if (value.gain <= 0.0f) {
        result.issue = ValidationIssue::GainNotPositive;
        return result;
    }
    if (policy == ValidationPolicy::StoredProfile &&
        value.gain > kMaximumStoredGainPerInputVolt) {
        result.issue = ValidationIssue::GainExceedsStorageLimit;
        return result;
    }
    if (!std::isfinite(value.offsetInputV)) {
        result.issue = ValidationIssue::OffsetNotFinite;
        return result;
    }
    if (value.offsetInputV < kAdcMinInputV) {
        result.issue = ValidationIssue::OffsetBelowAdcRange;
        result.limit = kAdcMinInputV;
        return result;
    }
    if (value.offsetInputV > kAdcMaxInputV) {
        result.issue = ValidationIssue::OffsetAboveAdcRange;
        result.limit = kAdcMaxInputV;
        return result;
    }
    result.outputRange = fullAdcOutputRange(value);
    if (!std::isfinite(result.outputRange.minimum) ||
        !std::isfinite(result.outputRange.maximum)) {
        result.issue = ValidationIssue::OutputNotFinite;
        return result;
    }
    if (policy == ValidationPolicy::StoredProfile) return result;

    const float magnitudeLimit =
        measurement == Measurement::Voltage ? kVoltageMaxV : kCurrentMaxA;
    if (result.outputRange.maximum > magnitudeLimit) {
        result.issue = ValidationIssue::OutputAboveSanityLimit;
        result.adcInputV = kAdcMaxInputV;
        result.impliedOutput = result.outputRange.maximum;
        result.limit = magnitudeLimit;
        return result;
    }
    if (result.outputRange.minimum < -magnitudeLimit) {
        result.issue = ValidationIssue::OutputBelowSanityLimit;
        result.adcInputV = kAdcMinInputV;
        result.impliedOutput = result.outputRange.minimum;
        result.limit = -magnitudeLimit;
        return result;
    }
    if (value.gain > kMaximumStoredGainPerInputVolt) {
        result.issue = ValidationIssue::GainExceedsStorageLimit;
    }
    return result;
}

enum class GainCalculationIssue : uint8_t {
    None,
    ReferenceNotFinite,
    ReferenceMustBePositive,
    ReferenceMustBeNonZero,
    ReferenceBelowSanityLimit,
    ReferenceAboveSanityLimit,
    InputNotFinite,
    DenominatorTooSmall,
    ReferenceSignMismatch,
    CandidateInvalid,
};

struct GainCalculationResult {
    GainCalculationIssue issue = GainCalculationIssue::None;
    Value candidate{};
    ValidationResult validation{};
    float reference = NAN;
    float referenceLimit = NAN;
    float denominator = NAN;

    bool accepted() const { return issue == GainCalculationIssue::None; }
};

inline GainCalculationResult calculateGain(
    Measurement measurement, Value staged, float reference, float inputV,
    int currentMultiplier = 1) {
    GainCalculationResult result{};
    result.candidate = staged;
    result.reference = reference;
    if (!std::isfinite(reference)) {
        result.issue = GainCalculationIssue::ReferenceNotFinite;
        return result;
    }
    if (measurement == Measurement::Voltage && reference <= 0.0f) {
        result.issue = GainCalculationIssue::ReferenceMustBePositive;
        return result;
    }
    if (measurement == Measurement::Current && reference == 0.0f) {
        result.issue = GainCalculationIssue::ReferenceMustBeNonZero;
        return result;
    }
    const float referenceMinimum =
        measurement == Measurement::Voltage ? kMinimumVoltageV
                                            : kMinimumCurrentA;
    const float referenceMaximum =
        measurement == Measurement::Voltage ? kMaximumVoltageV
                                            : kMaximumCurrentA;
    if (reference < referenceMinimum) {
        result.issue = GainCalculationIssue::ReferenceBelowSanityLimit;
        result.referenceLimit = referenceMinimum;
        return result;
    }
    if (reference > referenceMaximum) {
        result.issue = GainCalculationIssue::ReferenceAboveSanityLimit;
        result.referenceLimit = referenceMaximum;
        return result;
    }
    if (!std::isfinite(inputV)) {
        result.issue = GainCalculationIssue::InputNotFinite;
        return result;
    }
    result.denominator = inputV - staged.offsetInputV;
    if (measurement == Measurement::Current &&
        currentMultiplier == -1) {
        result.denominator = -result.denominator;
    }
    if (!std::isfinite(result.denominator) ||
        std::fabs(result.denominator) <
            kMinimumGainCalculationDenominatorV) {
        result.issue = GainCalculationIssue::DenominatorTooSmall;
        return result;
    }
    result.candidate.gain = reference / result.denominator;
    if (!std::isfinite(result.candidate.gain) ||
        result.candidate.gain <= 0.0f) {
        result.issue = GainCalculationIssue::ReferenceSignMismatch;
        return result;
    }
    result.validation = validate(
        measurement, result.candidate,
        ValidationPolicy::CommitCandidate);
    if (!result.validation.accepted()) {
        result.issue = GainCalculationIssue::CandidateInvalid;
    }
    return result;
}

void init();
// Calibration belongs to the physical Sensor 1/2/3 channel, so it follows the
// hardware if that channel is assigned to a different logical role.
Value defaults(Source source, uint8_t physicalSensor, Measurement measurement);
Value get(Source source, uint8_t physicalSensor, Measurement measurement);
bool set(Source source, uint8_t physicalSensor, Measurement measurement,
         Value value);
float apply(float inputV, Value value);

} // namespace sensors::calibration
