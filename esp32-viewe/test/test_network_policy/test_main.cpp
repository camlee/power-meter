#include <unity.h>

#include "network/network_policy.h"
#include "network/network_reconnect_policy.h"

#include <cstdint>

using network_manager::ConnectionFailure;

namespace {

void testOnlyCredentialFailuresRequireUserAction() {
    TEST_ASSERT_TRUE(network_manager::connectionFailureNeedsUserAction(
        ConnectionFailure::AuthenticationFailed));
    TEST_ASSERT_TRUE(network_manager::connectionFailureNeedsUserAction(
        ConnectionFailure::IncompatibleSecurity));

    TEST_ASSERT_FALSE(network_manager::connectionFailureNeedsUserAction(
        ConnectionFailure::NetworkNotFound));
    TEST_ASSERT_FALSE(network_manager::connectionFailureNeedsUserAction(
        ConnectionFailure::ConnectionFailed));
    TEST_ASSERT_FALSE(network_manager::connectionFailureNeedsUserAction(
        ConnectionFailure::TimedOut));
    TEST_ASSERT_FALSE(network_manager::connectionFailureNeedsUserAction(
        ConnectionFailure::LinkLost));
}

void testRetryPolicyMatchesFailureCategory() {
    TEST_ASSERT_FALSE(network_manager::connectionFailureShouldRetry(
        ConnectionFailure::None));
    TEST_ASSERT_FALSE(network_manager::connectionFailureShouldRetry(
        ConnectionFailure::AuthenticationFailed));
    TEST_ASSERT_FALSE(network_manager::connectionFailureShouldRetry(
        ConnectionFailure::IncompatibleSecurity));

    TEST_ASSERT_TRUE(network_manager::connectionFailureShouldRetry(
        ConnectionFailure::NetworkNotFound));
    TEST_ASSERT_TRUE(network_manager::connectionFailureShouldRetry(
        ConnectionFailure::ConnectionFailed));
    TEST_ASSERT_TRUE(network_manager::connectionFailureShouldRetry(
        ConnectionFailure::TimedOut));
    TEST_ASSERT_TRUE(network_manager::connectionFailureShouldRetry(
        ConnectionFailure::LinkLost));
}

void testReconnectDelayDoublesAndCaps() {
    constexpr uint32_t maximum = 60000;
    TEST_ASSERT_EQUAL_UINT32(2000,
        network_manager::nextReconnectDelay(1000, maximum));
    TEST_ASSERT_EQUAL_UINT32(32000,
        network_manager::nextReconnectDelay(16000, maximum));
    TEST_ASSERT_EQUAL_UINT32(maximum,
        network_manager::nextReconnectDelay(32000, maximum));
    TEST_ASSERT_EQUAL_UINT32(maximum,
        network_manager::nextReconnectDelay(maximum, maximum));
    TEST_ASSERT_EQUAL_UINT32(maximum,
        network_manager::nextReconnectDelay(UINT32_MAX, maximum));
}

void testRecoveryCandidateSelectionUsesSavedOrderAndSkipsIneligible() {
    network_manager::RecoveryCandidatePolicy candidates[] = {
        {true, false, false, 2, -30},
        {true, false, false, 0, -80},
        {true, false, false, 1, -40},
    };
    TEST_ASSERT_EQUAL_INT(1, network_manager::chooseRecoveryCandidate(candidates, 3));

    candidates[1].attempted = true;
    TEST_ASSERT_EQUAL_INT(2, network_manager::chooseRecoveryCandidate(candidates, 3));

    candidates[2].suppressed = true;
    TEST_ASSERT_EQUAL_INT(0, network_manager::chooseRecoveryCandidate(candidates, 3));

    candidates[0].visible = false;
    TEST_ASSERT_EQUAL_INT(-1, network_manager::chooseRecoveryCandidate(candidates, 3));
}

void testRecoveryCandidateUsesRssiAsTieBreaker() {
    network_manager::RecoveryCandidatePolicy candidates[] = {
        {true, false, false, 1, -72},
        {true, false, false, 1, -48},
    };
    TEST_ASSERT_EQUAL_INT(1, network_manager::chooseRecoveryCandidate(candidates, 2));
}

void testCredentialFailuresSuppressOnlyThatAutomaticCandidate() {
    TEST_ASSERT_TRUE(network_manager::failureSuppressesAutomaticCandidate(
        ConnectionFailure::AuthenticationFailed));
    TEST_ASSERT_TRUE(network_manager::failureSuppressesAutomaticCandidate(
        ConnectionFailure::IncompatibleSecurity));
    TEST_ASSERT_FALSE(network_manager::failureSuppressesAutomaticCandidate(
        ConnectionFailure::NetworkNotFound));
    TEST_ASSERT_FALSE(network_manager::failureSuppressesAutomaticCandidate(
        ConnectionFailure::TimedOut));
}

} // namespace

void setUp() {}
void tearDown() {}

int main() {
    UNITY_BEGIN();
    RUN_TEST(testOnlyCredentialFailuresRequireUserAction);
    RUN_TEST(testRetryPolicyMatchesFailureCategory);
    RUN_TEST(testReconnectDelayDoublesAndCaps);
    RUN_TEST(testRecoveryCandidateSelectionUsesSavedOrderAndSkipsIneligible);
    RUN_TEST(testRecoveryCandidateUsesRssiAsTieBreaker);
    RUN_TEST(testCredentialFailuresSuppressOnlyThatAutomaticCandidate);
    return UNITY_END();
}
