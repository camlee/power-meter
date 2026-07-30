#include <unity.h>

#include "sensors/sensor_mapping.h"

using sensors::mapping::CurrentDirection;
using sensors::mapping::LogicalRole;
using sensors::mapping::PhysicalSensorId;
using sensors::mapping::Profile;

void test_default_physical_order_is_solar_load_battery() {
    const Profile profile = sensors::mapping::defaults(sensor_mode::Mode::Adc);
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(LogicalRole::Solar),
        static_cast<uint8_t>(profile.physical[0].role));
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(LogicalRole::Load),
        static_cast<uint8_t>(profile.physical[1].role));
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(LogicalRole::Battery),
        static_cast<uint8_t>(profile.physical[2].role));
    TEST_ASSERT_TRUE(sensors::mapping::isValid(profile));
}

void test_adc_battery_default_preserves_phase_c_direction() {
    const Profile profile = sensors::mapping::defaults(sensor_mode::Mode::Adc);
    TEST_ASSERT_EQUAL_INT8(
        -1, sensors::mapping::multiplier(
                profile.physical[static_cast<uint8_t>(
                    PhysicalSensorId::Sensor3)].currentDirection));

    const sensor_mode::Mode unchangedModes[] = {
        sensor_mode::Mode::Uart,
        sensor_mode::Mode::Demo,
        sensor_mode::Mode::Ads1115,
    };
    for (const sensor_mode::Mode mode : unchangedModes) {
        const Profile unchanged = sensors::mapping::defaults(mode);
        for (const auto& physical : unchanged.physical) {
            TEST_ASSERT_EQUAL_INT8(
                1, sensors::mapping::multiplier(physical.currentDirection));
        }
    }
}

void test_battery_may_be_unmapped() {
    Profile profile = sensors::mapping::defaults(sensor_mode::Mode::Demo);
    profile.physical[2].role = LogicalRole::Unmapped;
    TEST_ASSERT_TRUE(sensors::mapping::isValid(profile));
}

void test_solar_and_load_are_required_and_roles_cannot_repeat() {
    Profile profile = sensors::mapping::defaults(sensor_mode::Mode::Demo);
    profile.physical[0].role = LogicalRole::Unmapped;
    TEST_ASSERT_FALSE(sensors::mapping::isValid(profile));

    profile = sensors::mapping::defaults(sensor_mode::Mode::Demo);
    profile.physical[2].role = LogicalRole::Solar;
    TEST_ASSERT_FALSE(sensors::mapping::isValid(profile));

    profile = sensors::mapping::defaults(sensor_mode::Mode::Demo);
    profile.physical[1].currentDirection =
        static_cast<CurrentDirection>(0);
    TEST_ASSERT_FALSE(sensors::mapping::isValid(profile));
}

void test_roles_translate_to_stable_logical_channels() {
    sensors::SensorId logical = sensors::SENSOR_COUNT;
    TEST_ASSERT_TRUE(sensors::mapping::logicalForRole(
        LogicalRole::Solar, logical));
    TEST_ASSERT_EQUAL_UINT8(sensors::SENSOR_SOLAR, logical);
    TEST_ASSERT_TRUE(sensors::mapping::logicalForRole(
        LogicalRole::Load, logical));
    TEST_ASSERT_EQUAL_UINT8(sensors::SENSOR_LOAD, logical);
    TEST_ASSERT_TRUE(sensors::mapping::logicalForRole(
        LogicalRole::Battery, logical));
    TEST_ASSERT_EQUAL_UINT8(sensors::SENSOR_BATTERY, logical);
    TEST_ASSERT_FALSE(sensors::mapping::logicalForRole(
        LogicalRole::Unmapped, logical));
}

void test_remapped_profile_resolves_physical_and_logical_channels() {
    Profile profile{{
        {LogicalRole::Battery, CurrentDirection::Reversed},
        {LogicalRole::Solar, CurrentDirection::Normal},
        {LogicalRole::Load, CurrentDirection::Normal},
    }};
    PhysicalSensorId physical = PhysicalSensorId::Sensor1;
    TEST_ASSERT_TRUE(sensors::mapping::physicalForLogical(
        profile, sensors::SENSOR_SOLAR, physical));
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(PhysicalSensorId::Sensor2),
        static_cast<uint8_t>(physical));

    sensors::SensorId logical = sensors::SENSOR_COUNT;
    TEST_ASSERT_TRUE(sensors::mapping::logicalForPhysical(
        profile, PhysicalSensorId::Sensor1, logical));
    TEST_ASSERT_EQUAL_UINT8(sensors::SENSOR_BATTERY, logical);
    TEST_ASSERT_EQUAL_INT8(
        -1, sensors::mapping::currentMultiplier(
                profile, PhysicalSensorId::Sensor1));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_default_physical_order_is_solar_load_battery);
    RUN_TEST(test_adc_battery_default_preserves_phase_c_direction);
    RUN_TEST(test_battery_may_be_unmapped);
    RUN_TEST(test_solar_and_load_are_required_and_roles_cannot_repeat);
    RUN_TEST(test_roles_translate_to_stable_logical_channels);
    RUN_TEST(test_remapped_profile_resolves_physical_and_logical_channels);
    return UNITY_END();
}
