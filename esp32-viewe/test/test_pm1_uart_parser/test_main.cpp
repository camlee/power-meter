#include <unity.h>

#include "sensors/pm1_uart_protocol.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

using sensors::pm1::Diagnostics;
using sensors::pm1::ParseError;
using sensors::pm1::Receiver;

namespace {

constexpr char kAuthoritativeFrame[] =
    "PM1,4182,2759012,03,18.24,2.13,0.742,13.17,1.06,,,,*7200\n";

std::string makeRecord(const std::string& payload, const char* ending = "\n") {
    char checksum[8];
    std::snprintf(checksum, sizeof(checksum), "*%04X", sensors::pm1::crc16CcittFalse(payload.data(), payload.size()));
    return payload + checksum + ending;
}

void feed(Receiver& receiver, const std::string& text, uint32_t nowMs) {
    receiver.feed(reinterpret_cast<const uint8_t*>(text.data()), text.size(), nowMs);
}

void testAuthoritativeFixtureAndStates() {
    const char* star = std::strchr(kAuthoritativeFrame, '*');
    TEST_ASSERT_NOT_NULL(star);
    TEST_ASSERT_EQUAL_HEX16(
        0x7200,
        sensors::pm1::crc16CcittFalse(
            kAuthoritativeFrame, static_cast<size_t>(star - kAuthoritativeFrame)));

    Receiver receiver;
    const size_t split = 19;
    receiver.feed(reinterpret_cast<const uint8_t*>(kAuthoritativeFrame), split, 100);
    TEST_ASSERT_TRUE(receiver.sample(0, 100).state == SensorSampleState::Waiting);
    TEST_ASSERT_FALSE(receiver.sample(0, 100).configured);
    receiver.feed(reinterpret_cast<const uint8_t*>(kAuthoritativeFrame + split),
                  std::strlen(kAuthoritativeFrame) - split, 250);

    const auto input = receiver.sample(0, 250);
    TEST_ASSERT_TRUE(input.state == SensorSampleState::Observed);
    TEST_ASSERT_TRUE(input.configured);
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 18.24f, input.voltage);
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 2.13f, input.current);
    TEST_ASSERT_TRUE(input.hasDutyCycle);
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.742f, input.dutyCycle);

    const auto output = receiver.sample(1, 250);
    TEST_ASSERT_TRUE(output.state == SensorSampleState::Observed);
    TEST_ASSERT_FALSE(output.hasDutyCycle);
    TEST_ASSERT_TRUE(receiver.sample(2, 250).state == SensorSampleState::NotConfigured);
    TEST_ASSERT_FALSE(receiver.sample(2, 250).configured);
    TEST_ASSERT_TRUE(receiver.sample(0, 2249).state == SensorSampleState::Observed);
    TEST_ASSERT_TRUE(receiver.sample(0, 2250).state == SensorSampleState::Stale);
    TEST_ASSERT_TRUE(receiver.sample(0, 2250).configured);

    const Diagnostics diagnostics = receiver.diagnostics();
    TEST_ASSERT_EQUAL_UINT32(1, diagnostics.validFrames);
    TEST_ASSERT_EQUAL_HEX8(0x03, diagnostics.mask);
    TEST_ASSERT_EQUAL_UINT32(4182, diagnostics.sequence);
    TEST_ASSERT_EQUAL_UINT32(2759012, diagnostics.sourceUptimeMs);
    TEST_ASSERT_EQUAL_UINT32(250, diagnostics.lastValidReceiveMs);
}

void testCrlfAndMalformedDoesNotEraseFreshFrame() {
    Receiver receiver;
    feed(receiver, makeRecord("PM1,1,500,01,12.5,-0.25,,,,,,,", "\r\n"), 1000);
    TEST_ASSERT_TRUE(receiver.sample(0, 1000).state == SensorSampleState::Observed);

    feed(receiver, "PM1,2,1000,01,13.0,1.0,,,,,,,,*0000\n", 1500);
    TEST_ASSERT_TRUE(receiver.sample(0, 2999).state == SensorSampleState::Observed);
    TEST_ASSERT_TRUE(receiver.sample(0, 3000).state == SensorSampleState::Stale);
    const Diagnostics diagnostics = receiver.diagnostics();
    TEST_ASSERT_EQUAL_UINT32(1, diagnostics.validFrames);
    TEST_ASSERT_EQUAL_UINT32(1, diagnostics.invalidFrames);
    TEST_ASSERT_EQUAL_UINT32(1, diagnostics.checksumErrors);
    TEST_ASSERT_TRUE(diagnostics.lastError == ParseError::Checksum);
}

void testStrictFieldsAndFiniteDecimalSyntax() {
    Receiver receiver;
    feed(receiver, makeRecord("PM1,1,2,01,12.0,1.0,,,,,,,"), 1);
    TEST_ASSERT_EQUAL_UINT32(1, receiver.diagnostics().validFrames);

    feed(receiver, makeRecord("PM1,2,3,01,12.0,,,,,,,,"), 2);
    feed(receiver, makeRecord("PM1,2,3,01,12.0,1e2,,,,,,,"), 2);
    feed(receiver, makeRecord("PM1,2,3,01,nan,1.0,,,,,,,"), 2);
    feed(receiver, makeRecord("PM1,2,3,01,12.0,1.0,,,9.0,,,,,"), 2);
    feed(receiver, makeRecord("PM1,2,3,08,,,,,,,,,"), 2);
    feed(receiver, makeRecord("PM1,2,3,1,12.0,1.0,,,,,,,"), 2);
    feed(receiver, makeRecord("PM1,2,3,01,12.0,1.0,,,,,,,,extra"), 2);
    feed(receiver, makeRecord("PM1,2,3,0a,12.0,1.0,,,,,,,"), 2);
    TEST_ASSERT_EQUAL_UINT32(1, receiver.diagnostics().validFrames);
    TEST_ASSERT_EQUAL_UINT32(8, receiver.diagnostics().invalidFrames);

    // Physical policy is intentionally not the parser's job. These finite
    // observations reach the shared sensor layer, which marks them ineligible.
    feed(receiver, makeRecord("PM1,2,3,01,121.0,-51.0,1.2,,,,,,"), 3);
    const auto sample = receiver.sample(0, 3);
    TEST_ASSERT_TRUE(sample.state == SensorSampleState::Observed);
    TEST_ASSERT_TRUE(sample.hasDutyCycle);
    TEST_ASSERT_EQUAL_FLOAT(1.2f, sample.dutyCycle);
}

void testOverflowAndRecovery() {
    Receiver receiver;
    const std::string overflow(160, 'X');
    feed(receiver, overflow + "\n", 10);
    TEST_ASSERT_EQUAL_UINT32(1, receiver.diagnostics().overflowFrames);
    TEST_ASSERT_EQUAL_UINT32(1, receiver.diagnostics().invalidFrames);
    TEST_ASSERT_TRUE(receiver.diagnostics().lastError == ParseError::Overflow);

    feed(receiver, makeRecord("PM1,9,20,00,,,,,,,,,"), 20);
    TEST_ASSERT_EQUAL_UINT32(1, receiver.diagnostics().validFrames);
    TEST_ASSERT_TRUE(receiver.sample(0, 20).state == SensorSampleState::NotConfigured);
}

void testSequenceDiagnosticsAndDuplicateFreshness() {
    Receiver receiver;
    feed(receiver, makeRecord("PM1,10,1000,01,12,1,,,,,,,"), 100);
    feed(receiver, makeRecord("PM1,10,1000,01,99,9,,,,,,,"), 1000);
    TEST_ASSERT_EQUAL_UINT32(1, receiver.diagnostics().duplicateFrames);
    TEST_ASSERT_EQUAL_UINT32(1, receiver.diagnostics().validFrames);
    TEST_ASSERT_TRUE(receiver.sample(0, 2099).state == SensorSampleState::Observed);
    TEST_ASSERT_TRUE(receiver.sample(0, 2100).state == SensorSampleState::Stale);
    TEST_ASSERT_EQUAL_FLOAT(12.0f, receiver.sample(0, 100).voltage);

    feed(receiver, makeRecord("PM1,13,2500,01,13,1,,,,,,,"), 2200);
    TEST_ASSERT_EQUAL_UINT32(1, receiver.diagnostics().sequenceGapEvents);
    TEST_ASSERT_EQUAL_UINT32(2, receiver.diagnostics().missingFrames);

    feed(receiver, makeRecord("PM1,1,20,01,14,1,,,,,,,"), 2300);
    TEST_ASSERT_EQUAL_UINT32(1, receiver.diagnostics().sequenceResets);

    Receiver wrapping;
    feed(wrapping, makeRecord("PM1,4294967295,4294967290,00,,,,,,,,,"), 1);
    feed(wrapping, makeRecord("PM1,0,4,00,,,,,,,,,"), 2);
    TEST_ASSERT_EQUAL_UINT32(1, wrapping.diagnostics().sequenceWraps);
    TEST_ASSERT_EQUAL_UINT32(0, wrapping.diagnostics().sequenceResets);
    TEST_ASSERT_EQUAL_UINT32(0, wrapping.diagnostics().missingFrames);
}

} // namespace

void setUp() {}
void tearDown() {}

int main() {
    UNITY_BEGIN();
    RUN_TEST(testAuthoritativeFixtureAndStates);
    RUN_TEST(testCrlfAndMalformedDoesNotEraseFreshFrame);
    RUN_TEST(testStrictFieldsAndFiniteDecimalSyntax);
    RUN_TEST(testOverflowAndRecovery);
    RUN_TEST(testSequenceDiagnosticsAndDuplicateFreshness);
    return UNITY_END();
}
