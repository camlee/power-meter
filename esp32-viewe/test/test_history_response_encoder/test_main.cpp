#include <unity.h>

#include "network/history_response_encoder.h"

#include <cmath>
#include <cstring>

namespace {

uint16_t readLe16(const uint8_t* data) {
    return static_cast<uint16_t>(data[0]) | static_cast<uint16_t>(data[1]) << 8;
}

uint32_t readLe32(const uint8_t* data) {
    return static_cast<uint32_t>(data[0]) |
           static_cast<uint32_t>(data[1]) << 8 |
           static_cast<uint32_t>(data[2]) << 16 |
           static_cast<uint32_t>(data[3]) << 24;
}

double readLeDouble(const uint8_t* data) {
    uint64_t bits = 0;
    for (uint8_t i = 0; i < 8; ++i) bits |= static_cast<uint64_t>(data[i]) << (i * 8);
    double value = 0;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

float readLeFloat(const uint8_t* data) {
    const uint32_t bits = readLe32(data);
    float value = 0;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

void testHeaderPreservesVph3WallClockLayout() {
    historical_storage::QueryStatus status{};
    status.startTimeMs = 1712345678000LL;
    status.endTimeMs = 1712349278000LL;
    status.timelineBasis = historical_storage::TimelineBasis::WallClock;
    status.incomplete = true;
    status.hasInferredTime = true;

    uint8_t output[history_response_encoder::kHeaderBytes];
    history_response_encoder::encodeHeader(output, 0x12345678, 336, status);

    TEST_ASSERT_EQUAL_HEX32(0x33485056, readLe32(output));
    TEST_ASSERT_EQUAL_UINT8(3, output[4]);
    TEST_ASSERT_EQUAL_UINT8(2, output[5]);
    TEST_ASSERT_EQUAL_HEX16(3, readLe16(output + 6));
    TEST_ASSERT_EQUAL_UINT16(336, readLe16(output + 8));
    TEST_ASSERT_EQUAL_UINT16(80, readLe16(output + 10));
    TEST_ASSERT_EQUAL_HEX32(0x12345678, readLe32(output + 12));
    TEST_ASSERT_TRUE(std::fabs(readLeDouble(output + 16) - static_cast<double>(status.startTimeMs)) < 0.5);
    TEST_ASSERT_TRUE(std::fabs(readLeDouble(output + 24) - static_cast<double>(status.endTimeMs)) < 0.5);
}

void testHeaderMarksCurrentSessionMonotonicTime() {
    historical_storage::QueryStatus status{};
    status.startTimeMs = -3600000;
    status.endTimeMs = 12345;
    status.timelineBasis = historical_storage::TimelineBasis::CurrentSessionMonotonic;

    uint8_t output[history_response_encoder::kHeaderBytes];
    history_response_encoder::encodeHeader(output, 7, 30, status);

    TEST_ASSERT_EQUAL_HEX16(4, readLe16(output + 6));
    TEST_ASSERT_TRUE(std::fabs(readLeDouble(output + 16) -
                              static_cast<double>(status.startTimeMs)) < 0.5);
}

void testRecordPreservesVph3WireLayoutAndZeroesReservedByte() {
    historical_storage::PowerBucket bucket{};
    bucket.startTimeMs = 1712345678000LL;
    bucket.coveredMs = 54321;
    bucket.configuredChannelMask = 0x05;
    bucket.timeFlags = 0x03;
    bucket.qualityFlags = 0x02;
    for (size_t i = 0; i < historical_storage::kSensorCount; ++i) {
        bucket.energyWh[i] = 10.5f + i;
        bucket.channelCoverageMs[i] = 1000 + i;
    }
    for (size_t i = 0; i < historical_storage::COMPONENT_COUNT; ++i) {
        bucket.componentEnergyWh[i] = 20.25f + i;
        bucket.componentCoverageMs[i] = 2000 + i;
    }

    uint8_t output[history_response_encoder::kRecordBytes];
    std::memset(output, 0xff, sizeof(output));
    history_response_encoder::encodeRecord(output, bucket);

    TEST_ASSERT_TRUE(std::fabs(readLeDouble(output) - static_cast<double>(bucket.startTimeMs)) < 0.5);
    TEST_ASSERT_EQUAL_UINT32(bucket.coveredMs, readLe32(output + 8));
    TEST_ASSERT_EQUAL_HEX8(0x05, output[12]);
    TEST_ASSERT_EQUAL_HEX8(0x03, output[13]);
    TEST_ASSERT_EQUAL_HEX8(0x02, output[14]);
    TEST_ASSERT_EQUAL_HEX8(0x00, output[15]);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, bucket.energyWh[2], readLeFloat(output + 24));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, bucket.componentEnergyWh[4], readLeFloat(output + 44));
    TEST_ASSERT_EQUAL_UINT32(bucket.channelCoverageMs[1], readLe32(output + 52));
    TEST_ASSERT_EQUAL_UINT32(bucket.componentCoverageMs[3], readLe32(output + 72));
}

} // namespace

void setUp() {}
void tearDown() {}

int main() {
    UNITY_BEGIN();
    RUN_TEST(testHeaderPreservesVph3WallClockLayout);
    RUN_TEST(testHeaderMarksCurrentSessionMonotonicTime);
    RUN_TEST(testRecordPreservesVph3WireLayoutAndZeroesReservedByte);
    return UNITY_END();
}
