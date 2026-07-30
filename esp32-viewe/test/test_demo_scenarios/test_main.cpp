#include <unity.h>

#include "sensors/demo_scenarios.h"

void test_schedule_advances_on_minute_boundaries_and_wraps() {
    TEST_ASSERT_EQUAL_UINT32(0, demo_scenarios::scenarioIndex(0));
    TEST_ASSERT_EQUAL_UINT32(0, demo_scenarios::scenarioIndex(59999));
    TEST_ASSERT_EQUAL_UINT32(1, demo_scenarios::scenarioIndex(60000));
    TEST_ASSERT_EQUAL_UINT32(2, demo_scenarios::scenarioIndex(120000));
    TEST_ASSERT_EQUAL_UINT32(3, demo_scenarios::scenarioIndex(180000));
    TEST_ASSERT_EQUAL_UINT32(4, demo_scenarios::scenarioIndex(240000));
    TEST_ASSERT_EQUAL_UINT32(0, demo_scenarios::scenarioIndex(300000));
}

void test_declared_balance_matches_present_demo_contract() {
    for (const auto& scenario : demo_scenarios::kScenarios) {
        const float calculated = scenario.channels[0].power -
                                 scenario.channels[1].power -
                                 scenario.channels[2].power;
        TEST_ASSERT_FLOAT_WITHIN(0.001f, scenario.expectedBalanceW, calculated);
    }
}

void test_schedule_exercises_charge_idle_discharge_and_mismatch() {
    TEST_ASSERT_GREATER_THAN_FLOAT(
        0.0f, demo_scenarios::kScenarios[0].channels[2].power);
    TEST_ASSERT_FLOAT_WITHIN(
        0.001f, 0.0f, demo_scenarios::kScenarios[1].channels[2].power);
    TEST_ASSERT_LESS_THAN_FLOAT(
        0.0f, demo_scenarios::kScenarios[2].channels[2].power);
    TEST_ASSERT_NOT_EQUAL_FLOAT(
        0.0f, demo_scenarios::kScenarios[3].expectedBalanceW);
}

void test_schedule_exercises_both_conflict_directions() {
    TEST_ASSERT_LESS_THAN_FLOAT(
        0.0f, demo_scenarios::kScenarios[3].expectedBalanceW);
    TEST_ASSERT_GREATER_THAN_FLOAT(
        0.0f, demo_scenarios::kScenarios[4].expectedBalanceW);
    TEST_ASSERT_GREATER_THAN_FLOAT(
        0.0f, demo_scenarios::kScenarios[3].channels[2].power);
    TEST_ASSERT_LESS_THAN_FLOAT(
        0.0f, demo_scenarios::kScenarios[4].channels[2].power);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_schedule_advances_on_minute_boundaries_and_wraps);
    RUN_TEST(test_declared_balance_matches_present_demo_contract);
    RUN_TEST(test_schedule_exercises_charge_idle_discharge_and_mismatch);
    RUN_TEST(test_schedule_exercises_both_conflict_directions);
    return UNITY_END();
}
