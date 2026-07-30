#include <cmath>

#include <unity.h>

#include "data/power_flow.h"

void assertTotals(const power_flow::UsageBreakdown& flow) {
    TEST_ASSERT_FLOAT_WITHIN(0.001f, flow.solarTotal,
                             flow.charge + flow.solarRemainder);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, flow.loadTotal,
                             flow.loadRemainder + flow.discharge);
}

void assertRange(const power_flow::SegmentRange& range, float from, float to) {
    TEST_ASSERT_FLOAT_WITHIN(0.001f, from, range.from);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, to, range.to);
}

void test_day_charge_is_part_of_the_positive_solar_total() {
    const auto flow = power_flow::usage(40.0f, 14.0f, 26.0f);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 26.0f, flow.charge);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 14.0f, flow.solarRemainder);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 14.0f, flow.loadRemainder);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, flow.discharge);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, flow.balance);
    assertRange(flow.chargeSegment, 0.0f, 26.0f);
    assertRange(flow.solarSegment, 26.0f, 40.0f);
    assertRange(flow.loadSegment, 0.0f, -14.0f);
    assertTotals(flow);
}

void test_solar_direct_has_no_battery_segment() {
    const auto flow = power_flow::usage(18.0f, 18.0f, 0.0f);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, flow.charge);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 18.0f, flow.solarRemainder);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 18.0f, flow.loadRemainder);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, flow.discharge);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, flow.balance);
    assertTotals(flow);
}

void test_night_discharge_is_part_of_the_negative_load_total() {
    const auto flow = power_flow::usage(0.0f, 16.0f, -16.0f);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, flow.charge);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, flow.solarRemainder);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, flow.loadRemainder);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 16.0f, flow.discharge);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, flow.balance);
    assertRange(flow.dischargeSegment, 0.0f, -16.0f);
    assertTotals(flow);
}

void test_positive_balance_is_the_outermost_tip() {
    const auto flow = power_flow::usage(40.0f, 20.0f, 15.0f);
    assertRange(flow.chargeSegment, 0.0f, 15.0f);
    assertRange(flow.solarSegment, 15.0f, 35.0f);
    assertRange(flow.balanceSegment, 35.0f, 40.0f);
    assertRange(flow.loadSegment, 0.0f, -20.0f);
}

void test_negative_balance_is_the_outermost_tip() {
    const auto flow = power_flow::usage(30.0f, 20.0f, 15.0f);
    assertRange(flow.chargeSegment, 0.0f, 15.0f);
    assertRange(flow.solarSegment, 15.0f, 30.0f);
    assertRange(flow.loadSegment, 0.0f, -15.0f);
    assertRange(flow.balanceSegment, -15.0f, -20.0f);
}

void test_charge_conflict_uses_the_full_floating_balance_band() {
    const auto flow = power_flow::usage(20.0f, 6.0f, 22.0f);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 22.0f, flow.charge);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, flow.solarRemainder);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, flow.loadRemainder);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, flow.discharge);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, -8.0f, flow.balance);
    TEST_ASSERT_TRUE(flow.conflict);
    assertRange(flow.loadSegment, -2.0f, -8.0f);
    assertRange(flow.balanceSegment, -2.0f, 6.0f);
    assertRange(flow.chargeSegment, 6.0f, 28.0f);
    TEST_ASSERT_TRUE(std::isnan(flow.solarSegment.from));
}

void test_discharge_conflict_is_the_mirrored_floating_stack() {
    const auto flow = power_flow::usage(6.0f, 20.0f, -22.0f);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 8.0f, flow.balance);
    TEST_ASSERT_TRUE(flow.conflict);
    assertRange(flow.dischargeSegment, -6.0f, -28.0f);
    assertRange(flow.balanceSegment, -6.0f, 2.0f);
    assertRange(flow.solarSegment, 2.0f, 8.0f);
    TEST_ASSERT_TRUE(std::isnan(flow.loadSegment.from));
}

void test_missing_battery_preserves_measured_solar_and_load_totals() {
    const auto flow = power_flow::usage(12.0f, 5.0f, NAN);
    TEST_ASSERT_TRUE(std::isnan(flow.charge));
    TEST_ASSERT_TRUE(std::isnan(flow.discharge));
    TEST_ASSERT_TRUE(std::isnan(flow.balance));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 12.0f, flow.solarRemainder);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 5.0f, flow.loadRemainder);
}

void test_inferred_battery_subdivides_history_without_claiming_balance() {
    const auto flow = power_flow::usage(12.0f, 5.0f, 7.0f, false);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 7.0f, flow.charge);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 5.0f, flow.solarRemainder);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 5.0f, flow.loadRemainder);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, flow.discharge);
    TEST_ASSERT_TRUE(std::isnan(flow.balance));
    TEST_ASSERT_FALSE(flow.conflict);
    assertRange(flow.chargeSegment, 0.0f, 7.0f);
    assertRange(flow.solarSegment, 7.0f, 12.0f);
    assertRange(flow.loadSegment, 0.0f, -5.0f);
    TEST_ASSERT_TRUE(std::isnan(flow.balanceSegment.from));
    assertTotals(flow);
}

void test_hidden_balance_keeps_measurement_totals_without_conflict_extensions() {
    const auto flow = power_flow::usage(20.0f, 6.0f, 22.0f, true, false);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 20.0f, flow.charge);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, flow.solarRemainder);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 6.0f, flow.loadRemainder);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, flow.discharge);
    TEST_ASSERT_FALSE(flow.conflict);
    assertRange(flow.chargeSegment, 0.0f, 20.0f);
    assertRange(flow.loadSegment, 0.0f, -6.0f);
    TEST_ASSERT_TRUE(std::isnan(flow.balanceSegment.from));
    assertTotals(flow);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_day_charge_is_part_of_the_positive_solar_total);
    RUN_TEST(test_solar_direct_has_no_battery_segment);
    RUN_TEST(test_night_discharge_is_part_of_the_negative_load_total);
    RUN_TEST(test_positive_balance_is_the_outermost_tip);
    RUN_TEST(test_negative_balance_is_the_outermost_tip);
    RUN_TEST(test_charge_conflict_uses_the_full_floating_balance_band);
    RUN_TEST(test_discharge_conflict_is_the_mirrored_floating_stack);
    RUN_TEST(test_missing_battery_preserves_measured_solar_and_load_totals);
    RUN_TEST(test_inferred_battery_subdivides_history_without_claiming_balance);
    RUN_TEST(test_hidden_balance_keeps_measurement_totals_without_conflict_extensions);
    return UNITY_END();
}
