#include <unity.h>
#include "network/wifi_credential_policy.h"

using network_manager::ApPasswordAction;
using network_manager::ApSettingsValidation;
using network_manager::validateApSettings;

void test_keep_requires_a_saved_secured_password() {
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(ApSettingsValidation::Valid),
        static_cast<int>(validateApSettings(
            "meter-ap", true, ApPasswordAction::Keep, nullptr, true)));
    TEST_ASSERT_NOT_EQUAL(
        static_cast<int>(ApSettingsValidation::Valid),
        static_cast<int>(validateApSettings(
            "meter-ap", true, ApPasswordAction::Keep, nullptr, false)));
    TEST_ASSERT_NOT_EQUAL(
        static_cast<int>(ApSettingsValidation::Valid),
        static_cast<int>(validateApSettings(
            "meter-ap", false, ApPasswordAction::Keep, nullptr, true)));
    TEST_ASSERT_NOT_EQUAL(
        static_cast<int>(ApSettingsValidation::Valid),
        static_cast<int>(validateApSettings(
            "meter-ap", true, ApPasswordAction::Keep, "contradiction", true)));
}

void test_replace_accepts_only_8_through_63_byte_passwords() {
    const char password8[] = "12345678";
    const char password63[] =
        "123456789012345678901234567890123456789012345678901234567890123";
    const char password64[] =
        "1234567890123456789012345678901234567890123456789012345678901234";

    TEST_ASSERT_NOT_EQUAL(
        static_cast<int>(ApSettingsValidation::Valid),
        static_cast<int>(validateApSettings(
            "meter-ap", true, ApPasswordAction::Replace, "1234567", false)));
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(ApSettingsValidation::Valid),
        static_cast<int>(validateApSettings(
            "meter-ap", true, ApPasswordAction::Replace, password8, false)));
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(ApSettingsValidation::Valid),
        static_cast<int>(validateApSettings(
            "meter-ap", true, ApPasswordAction::Replace, password63, false)));
    TEST_ASSERT_NOT_EQUAL(
        static_cast<int>(ApSettingsValidation::Valid),
        static_cast<int>(validateApSettings(
            "meter-ap", true, ApPasswordAction::Replace, password64, false)));
    TEST_ASSERT_NOT_EQUAL(
        static_cast<int>(ApSettingsValidation::Valid),
        static_cast<int>(validateApSettings(
            "meter-ap", false, ApPasswordAction::Replace, password8, false)));
}

void test_remove_is_only_valid_for_an_open_ap() {
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(ApSettingsValidation::Valid),
        static_cast<int>(validateApSettings(
            "meter-ap", false, ApPasswordAction::Remove, nullptr, true)));
    TEST_ASSERT_NOT_EQUAL(
        static_cast<int>(ApSettingsValidation::Valid),
        static_cast<int>(validateApSettings(
            "meter-ap", true, ApPasswordAction::Remove, nullptr, true)));
    TEST_ASSERT_NOT_EQUAL(
        static_cast<int>(ApSettingsValidation::Valid),
        static_cast<int>(validateApSettings(
            "meter-ap", false, ApPasswordAction::Remove, "contradiction", true)));
}

void test_ssid_and_control_characters_are_validated_centrally() {
    TEST_ASSERT_NOT_EQUAL(
        static_cast<int>(ApSettingsValidation::Valid),
        static_cast<int>(validateApSettings(
            "", false, ApPasswordAction::Remove, nullptr, false)));
    TEST_ASSERT_NOT_EQUAL(
        static_cast<int>(ApSettingsValidation::Valid),
        static_cast<int>(validateApSettings(
            "bad\nssid", false, ApPasswordAction::Remove, nullptr, false)));
    TEST_ASSERT_NOT_EQUAL(
        static_cast<int>(ApSettingsValidation::Valid),
        static_cast<int>(validateApSettings(
            "meter-ap", true, ApPasswordAction::Replace, "bad\npassword", false)));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_keep_requires_a_saved_secured_password);
    RUN_TEST(test_replace_accepts_only_8_through_63_byte_passwords);
    RUN_TEST(test_remove_is_only_valid_for_an_open_ap);
    RUN_TEST(test_ssid_and_control_characters_are_validated_centrally);
    return UNITY_END();
}
