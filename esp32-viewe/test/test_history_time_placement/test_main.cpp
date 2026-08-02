#include <unity.h>

#include "data/history_time_placement.h"

using history_time_placement::Kind;
using history_time_placement::Session;
using history_time_placement::kMinuteMs;

namespace {

void testInfersSeveralSessionsAsOneBoundedBlock() {
    Session sessions[] = {
        {1, 0, 10, 0, Kind::Direct},
        {2, 2, 12, 0, Kind::Unresolved},
        {3, 0, 5, 0, Kind::Unresolved},
        {4, 3, 23, 0, Kind::Unresolved},
        {5, 0, 10, 49 * kMinuteMs, Kind::Direct},
    };
    history_time_placement::inferBounded(sessions, 5, 4 * kMinuteMs);

    TEST_ASSERT_EQUAL_INT(static_cast<int>(Kind::Inferred), static_cast<int>(sessions[1].kind));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Kind::Inferred), static_cast<int>(sessions[2].kind));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Kind::Inferred), static_cast<int>(sessions[3].kind));
    TEST_ASSERT_EQUAL_INT64(11 * kMinuteMs, sessions[1].startTimeMs);
    TEST_ASSERT_EQUAL_INT64(22 * kMinuteMs, sessions[2].startTimeMs);
    TEST_ASSERT_EQUAL_INT64(28 * kMinuteMs, sessions[3].startTimeMs);
}

void testRejectsBlockBeyondTolerance() {
    Session sessions[] = {
        {1, 0, 10, 0, Kind::Direct},
        {2, 0, 10, 0, Kind::Unresolved},
        {3, 0, 10, 23 * kMinuteMs, Kind::Direct},
    };
    history_time_placement::inferBounded(sessions, 3, 2 * kMinuteMs);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Kind::Unresolved), static_cast<int>(sessions[1].kind));
}

void testAssumesOneMinuteGapsBackwardFromLaterPlacement() {
    Session sessions[] = {
        {1, 0, 10, 0, Kind::Unresolved},
        {2, 0, 5, 0, Kind::Unresolved},
        {3, 0, 1, 0, Kind::Direct},
    };
    history_time_placement::assumeUnresolved(sessions, 3, kMinuteMs);

    TEST_ASSERT_EQUAL_INT64(-17 * kMinuteMs, sessions[0].startTimeMs);
    TEST_ASSERT_EQUAL_INT64(-6 * kMinuteMs, sessions[1].startTimeMs);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Kind::Assumed), static_cast<int>(sessions[0].kind));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Kind::Assumed), static_cast<int>(sessions[1].kind));
}

void testAssumesForwardWhenOnlyEarlierPlacementExists() {
    Session sessions[] = {
        {1, 0, 10, 100 * kMinuteMs, Kind::Direct},
        {2, 0, 5, 0, Kind::Unresolved},
        {3, 0, 2, 0, Kind::Unresolved},
    };
    history_time_placement::assumeUnresolved(sessions, 3, kMinuteMs);

    TEST_ASSERT_EQUAL_INT64(111 * kMinuteMs, sessions[1].startTimeMs);
    TEST_ASSERT_EQUAL_INT64(117 * kMinuteMs, sessions[2].startTimeMs);
}

void testDoesNotOverlapFixedEndpointsToForceAssumption() {
    Session sessions[] = {
        {1, 0, 10, 0, Kind::Direct},
        {2, 0, 10, 0, Kind::Unresolved},
        {3, 0, 10, 20 * kMinuteMs, Kind::Direct},
    };
    history_time_placement::assumeUnresolved(sessions, 3, kMinuteMs);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Kind::Unresolved), static_cast<int>(sessions[1].kind));
}

} // namespace

void setUp() {}
void tearDown() {}

int main() {
    UNITY_BEGIN();
    RUN_TEST(testInfersSeveralSessionsAsOneBoundedBlock);
    RUN_TEST(testRejectsBlockBeyondTolerance);
    RUN_TEST(testAssumesOneMinuteGapsBackwardFromLaterPlacement);
    RUN_TEST(testAssumesForwardWhenOnlyEarlierPlacementExists);
    RUN_TEST(testDoesNotOverlapFixedEndpointsToForceAssumption);
    return UNITY_END();
}
