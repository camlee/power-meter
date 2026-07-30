#include <unity.h>

#include "sensors/adc_window_reducer.h"

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
    reducer.add(reading(10.0f, 100.0f, sensors::ReadingState::OutOfRange));

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
            reading(10.0f, 100.0f, sensors::ReadingState::OutOfRange));
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
            reading(12.0f, 60.0f, sensors::ReadingState::OutOfRange));
    }

    const sensors::Reading result = reducer.finish(500);
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(sensors::ReadingState::OutOfRange),
        static_cast<int>(result.state));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 720.0f, result.power);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(testIsolatedOutlierIsExcludedAndWindowRemainsValid);
    RUN_TEST(testTooManyRejectedSamplesInvalidateWindow);
    RUN_TEST(testContinuousOutOfRangeStillReportsDiagnosticValues);
    return UNITY_END();
}
