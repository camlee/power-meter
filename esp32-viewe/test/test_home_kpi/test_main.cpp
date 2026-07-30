#include <unity.h>

#include "ui/home_kpi.h"

#include <cmath>

void setUp() {}
void tearDown() {}

void test_regular_update_is_four_point_average() {
    home_kpi::AdaptiveFilter filter;
    TEST_ASSERT_TRUE(filter.add(0, 10.0f).publish);
    TEST_ASSERT_FALSE(filter.add(500, 11.0f).publish);
    TEST_ASSERT_FALSE(filter.add(1000, 9.0f).publish);
    TEST_ASSERT_FALSE(filter.add(1500, 10.0f).publish);
    const auto result = filter.add(2000, 12.0f);
    TEST_ASSERT_TRUE(result.publish);
    TEST_ASSERT_TRUE(result.available);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 10.5f, result.value);
}

void test_step_change_publishes_after_two_samples() {
    home_kpi::AdaptiveFilter filter;
    filter.add(0, 10.0f);
    TEST_ASSERT_FALSE(filter.add(500, 30.0f).publish);
    const auto result = filter.add(1000, 32.0f);
    TEST_ASSERT_TRUE(result.publish);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 31.0f, result.value);
}

void test_single_spike_does_not_publish_early() {
    home_kpi::AdaptiveFilter filter;
    filter.add(0, 10.0f);
    TEST_ASSERT_FALSE(filter.add(500, 30.0f).publish);
    TEST_ASSERT_FALSE(filter.add(1000, 11.0f).publish);
}

void test_relative_threshold_handles_large_values() {
    home_kpi::AdaptiveFilter filter;
    filter.add(0, 200.0f);
    TEST_ASSERT_FALSE(filter.add(500, 207.0f).publish);
    TEST_ASSERT_FALSE(filter.add(1000, 208.0f).publish);
    TEST_ASSERT_FALSE(filter.add(1500, 220.0f).publish);
    TEST_ASSERT_TRUE(filter.add(2000, 222.0f).publish);
}

void test_two_missing_samples_publish_unavailable() {
    home_kpi::AdaptiveFilter filter;
    filter.add(0, 10.0f);
    TEST_ASSERT_FALSE(filter.add(500, NAN).publish);
    const auto result = filter.add(1000, NAN);
    TEST_ASSERT_TRUE(result.publish);
    TEST_ASSERT_FALSE(result.available);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_regular_update_is_four_point_average);
    RUN_TEST(test_step_change_publishes_after_two_samples);
    RUN_TEST(test_single_spike_does_not_publish_early);
    RUN_TEST(test_relative_threshold_handles_large_values);
    RUN_TEST(test_two_missing_samples_publish_unavailable);
    return UNITY_END();
}
