#include <unity.h>

#include "network/semver.h"
#include "network/update_policy.h"

namespace {

void testSemverValidation() {
    TEST_ASSERT_TRUE(semver::isValid("0.1.0"));
    TEST_ASSERT_TRUE(semver::isValid("1.2.3-rc.1+build.7"));
    TEST_ASSERT_FALSE(semver::isValid("v1.2.3"));
    TEST_ASSERT_FALSE(semver::isValid("1.02.3"));
    TEST_ASSERT_FALSE(semver::isValid("1.2"));
    TEST_ASSERT_FALSE(semver::isValid("1.2.3-"));
    TEST_ASSERT_TRUE(semver::isStable("1.2.3+build.7"));
    TEST_ASSERT_FALSE(semver::isStable("1.2.3-rc.1"));
}

void testSemverPrecedence() {
    bool valid = false;
    TEST_ASSERT_EQUAL_INT(-1, semver::compare("1.2.9", "1.10.0", valid));
    TEST_ASSERT_TRUE(valid);
    TEST_ASSERT_EQUAL_INT(-1, semver::compare("1.0.0-rc.1", "1.0.0", valid));
    TEST_ASSERT_EQUAL_INT(-1, semver::compare("1.0.0-beta.2", "1.0.0-beta.11", valid));
    TEST_ASSERT_EQUAL_INT(1, semver::compare("1.0.0-1a", "1.0.0-99", valid));
    TEST_ASSERT_EQUAL_INT(0, semver::compare("1.0.0+one", "1.0.0+two", valid));
    TEST_ASSERT_EQUAL_INT(1, semver::compare("2.0.0", "1.99.99", valid));
}

void testAutomaticCadenceAndBackoff() {
    TEST_ASSERT_TRUE(update_policy::automaticCheckDue(false, 0, 100));
    TEST_ASSERT_FALSE(update_policy::automaticCheckDue(true, 100, 100 + 86399));
    TEST_ASSERT_TRUE(update_policy::automaticCheckDue(true, 100, 100 + 86400));
    TEST_ASSERT_TRUE(update_policy::automaticCheckDue(true, 200, 100));
    TEST_ASSERT_EQUAL_UINT32(300000, update_policy::retryDelayMs(1));
    TEST_ASSERT_EQUAL_UINT32(900000, update_policy::retryDelayMs(2));
    TEST_ASSERT_EQUAL_UINT32(3600000, update_policy::retryDelayMs(3));
    TEST_ASSERT_EQUAL_UINT32(21600000, update_policy::retryDelayMs(8));
}

} // namespace

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(testSemverValidation);
    RUN_TEST(testSemverPrecedence);
    RUN_TEST(testAutomaticCadenceAndBackoff);
    return UNITY_END();
}
