#include <unity.h>

#include "sensors/adc_window_reducer.h"
#include "sensors/sensor_calibration.h"

namespace {

sensors::Reading reading(
    float voltage, float current,
    sensors::ReadingState state = sensors::ReadingState::Valid) {
    sensors::Reading result;
    result.configured = true;
    result.state = state;
    result.voltageInputV = voltage / 37.0f;
    result.currentInputV = current / 37.0f;
    result.voltage = voltage;
    result.current = current;
    result.power = voltage * current;
    return result;
}

} // namespace

void setUp() {}
void tearDown() {}

void testIsolatedOutlierIsExcludedAndWindowRemainsValid() {
    sensors::AdcWindowReducer reducer;
    for (int i = 0; i < 9; ++i) reducer.add(reading(10.0f, 2.0f));
    reducer.add(reading(10.0f, 200.0f, sensors::ReadingState::OutOfRange));

    sensors::AdcWindowQuality quality{};
    const sensors::Reading result = reducer.finish(500, &quality);
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(sensors::ReadingState::Valid),
        static_cast<int>(result.state));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 20.0f, result.power);
    TEST_ASSERT_EQUAL_UINT16(9, quality.validSampleCount);
    TEST_ASSERT_EQUAL_UINT16(1, quality.rejectedSampleCount);
    TEST_ASSERT_TRUE(quality.toleratedRejections);
}

void testTooManyRejectedSamplesInvalidateWindow() {
    sensors::AdcWindowReducer reducer;
    for (int i = 0; i < 7; ++i) reducer.add(reading(10.0f, 2.0f));
    for (int i = 0; i < 3; ++i) {
        reducer.add(
            reading(10.0f, 200.0f, sensors::ReadingState::OutOfRange));
    }

    const sensors::Reading result = reducer.finish(500);
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(sensors::ReadingState::OutOfRange),
        static_cast<int>(result.state));
    TEST_ASSERT_TRUE(std::isfinite(result.power));
}

void testContinuousOutOfRangeStillReportsDiagnosticValues() {
    sensors::AdcWindowReducer reducer;
    for (int i = 0; i < 10; ++i) {
        reducer.add(
            reading(12.0f, 160.0f, sensors::ReadingState::OutOfRange));
    }

    const sensors::Reading result = reducer.finish(500);
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(sensors::ReadingState::OutOfRange),
        static_cast<int>(result.state));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 1920.0f, result.power);
}

void testRelaxedSanityLimitsIncludeExpectedInstallationRange() {
    TEST_ASSERT_TRUE(sensors::isPlausibleVoltage(250.0f));
    TEST_ASSERT_FALSE(sensors::isPlausibleVoltage(250.01f));
    TEST_ASSERT_TRUE(sensors::isPlausibleCurrent(-150.0f));
    TEST_ASSERT_TRUE(sensors::isPlausibleCurrent(150.0f));
    TEST_ASSERT_FALSE(sensors::isPlausibleCurrent(-150.01f));
    TEST_ASSERT_FALSE(sensors::isPlausibleCurrent(150.01f));
}

void testCalibrationValidationUsesFullAdcRangeNotLiveReading() {
    using sensors::calibration::Measurement;
    using sensors::calibration::ValidationIssue;
    using sensors::calibration::Value;

    struct Case {
        Measurement measurement;
        Value value;
        ValidationIssue issue;
    };
    const Case cases[] = {
        {Measurement::Voltage, {21.3f, 0.01f}, ValidationIssue::None},
        {Measurement::Current, {33.0f, 1.1613f}, ValidationIssue::None},
        {Measurement::Voltage, {250.0f / 3.3f, 0.0f},
         ValidationIssue::None},
        {Measurement::Voltage, {250.01f / 3.3f, 0.0f},
         ValidationIssue::OutputAboveSanityLimit},
        {Measurement::Current, {150.0f / 3.3f, 3.3f},
         ValidationIssue::None},
        {Measurement::Current, {150.01f / 3.3f, 3.3f},
         ValidationIssue::OutputBelowSanityLimit},
        {Measurement::Voltage, {100.0f, 0.0f},
         ValidationIssue::OutputAboveSanityLimit},
        {Measurement::Current, {100.0f, 1.65f},
         ValidationIssue::OutputAboveSanityLimit},
        {Measurement::Current, {100.0f, 3.3f},
         ValidationIssue::OutputBelowSanityLimit},
        {Measurement::Voltage, {21.3f, -0.01f},
         ValidationIssue::OffsetBelowAdcRange},
        {Measurement::Voltage, {21.3f, 3.31f},
         ValidationIssue::OffsetAboveAdcRange},
        {Measurement::Voltage, {0.0f, 0.0f},
         ValidationIssue::GainNotPositive},
        {Measurement::Voltage, {NAN, 0.0f},
         ValidationIssue::GainNotFinite},
    };
    for (const Case& test : cases) {
        const auto validation =
            sensors::calibration::validate(
                test.measurement, test.value);
        TEST_ASSERT_EQUAL_INT(
            static_cast<int>(test.issue),
            static_cast<int>(validation.issue));
    }

    const auto voltageHigh = sensors::calibration::validate(
        Measurement::Voltage, Value{100.0f, 0.0f});
    TEST_ASSERT_FLOAT_WITHIN(
        0.001f, 330.0f, voltageHigh.impliedOutput);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 250.0f, voltageHigh.limit);

    const auto currentLow = sensors::calibration::validate(
        Measurement::Current, Value{100.0f, 3.3f});
    TEST_ASSERT_FLOAT_WITHIN(
        0.001f, -330.0f, currentLow.impliedOutput);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, -150.0f, currentLow.limit);
}

void testStoredProfilePolicyPreservesLegacyCalibration() {
    using sensors::calibration::Measurement;
    using sensors::calibration::ValidationIssue;
    using sensors::calibration::ValidationPolicy;
    using sensors::calibration::Value;

    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(ValidationIssue::None),
        static_cast<int>(sensors::calibration::validate(
            Measurement::Voltage, Value{1000.0f, 0.0f},
            ValidationPolicy::StoredProfile).issue));
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(ValidationIssue::GainExceedsStorageLimit),
        static_cast<int>(sensors::calibration::validate(
            Measurement::Voltage, Value{10001.0f, 0.0f},
            ValidationPolicy::StoredProfile).issue));
}

void testGainCalculationReturnsStructuredIssues() {
    using sensors::calibration::GainCalculationIssue;
    using sensors::calibration::Measurement;
    using sensors::calibration::ValidationIssue;
    using sensors::calibration::Value;

    struct Case {
        Measurement measurement;
        Value staged;
        float reference;
        float input;
        int multiplier;
        GainCalculationIssue issue;
    };
    const Case cases[] = {
        {Measurement::Voltage, {21.3f, 0.0f}, 20.0f, 1.0f, 1,
         GainCalculationIssue::None},
        {Measurement::Voltage, {21.3f, 0.0f}, -1.0f, 1.0f, 1,
         GainCalculationIssue::ReferenceMustBePositive},
        {Measurement::Current, {40.0f, 1.667f}, -10.0f, 1.417f, 1,
         GainCalculationIssue::None},
        {Measurement::Current, {40.0f, 1.667f}, 10.0f, 1.417f, 1,
         GainCalculationIssue::ReferenceSignMismatch},
        {Measurement::Current, {40.0f, 1.667f}, 10.0f, 1.417f, -1,
         GainCalculationIssue::None},
        {Measurement::Current, {40.0f, 1.667f}, 0.0f, 1.417f, 1,
         GainCalculationIssue::ReferenceMustBeNonZero},
        {Measurement::Current, {40.0f, 1.65f}, -150.0f, 0.0f, 1,
         GainCalculationIssue::None},
        {Measurement::Current, {40.0f, 1.65f}, -150.01f, 0.0f, 1,
         GainCalculationIssue::ReferenceBelowSanityLimit},
        {Measurement::Voltage, {21.3f, 0.0f}, 251.0f, 1.0f, 1,
         GainCalculationIssue::ReferenceAboveSanityLimit},
        {Measurement::Voltage, {21.3f, 0.0f}, 20.0f, 0.001f, 1,
         GainCalculationIssue::DenominatorTooSmall},
        {Measurement::Voltage, {21.3f, 0.0f}, 140.0f, 0.1f, 1,
         GainCalculationIssue::CandidateInvalid},
    };
    for (const Case& test : cases) {
        const auto calculation = sensors::calibration::calculateGain(
            test.measurement, test.staged, test.reference,
            test.input, test.multiplier);
        TEST_ASSERT_EQUAL_INT(
            static_cast<int>(test.issue),
            static_cast<int>(calculation.issue));
    }

    const auto invalidCandidate = sensors::calibration::calculateGain(
        Measurement::Voltage, Value{21.3f, 0.0f}, 140.0f, 0.1f);
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(ValidationIssue::OutputAboveSanityLimit),
        static_cast<int>(invalidCandidate.validation.issue));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(testIsolatedOutlierIsExcludedAndWindowRemainsValid);
    RUN_TEST(testTooManyRejectedSamplesInvalidateWindow);
    RUN_TEST(testContinuousOutOfRangeStillReportsDiagnosticValues);
    RUN_TEST(testRelaxedSanityLimitsIncludeExpectedInstallationRange);
    RUN_TEST(testCalibrationValidationUsesFullAdcRangeNotLiveReading);
    RUN_TEST(testStoredProfilePolicyPreservesLegacyCalibration);
    RUN_TEST(testGainCalculationReturnsStructuredIssues);
    return UNITY_END();
}
