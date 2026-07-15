#include <unity.h>

#include "pm1_protocol.h"

#include <string.h>

namespace {

void testCrc16CcittFalse(){
    TEST_ASSERT_EQUAL_HEX16(0x29B1, pm1Crc16CcittFalse("123456789", 9));
}

void testFormatsAuthoritativeRecord(){
    const char *fields[PM1_FIELD_COUNT] = {
        "18.24", "2.13", "0.742",
        "13.17", "1.06", "",
        "", "", ""
    };
    char record[PM1_MAX_RECORD_LENGTH];
    const size_t length = pm1FormatRecord(
        record, sizeof(record), 4182, 2759012, 0x03, fields);
    const char *expected =
        "PM1,4182,2759012,03,18.24,2.13,0.742,13.17,1.06,,,,*7200\n";

    TEST_ASSERT_EQUAL_UINT(strlen(expected), length);
    TEST_ASSERT_EQUAL_STRING(expected, record);
}

void testRejectsInvalidDestinationAndMask(){
    const char *fields[PM1_FIELD_COUNT] = {
        "18.24", "2.13", "0.742",
        "13.17", "1.06", "",
        "", "", ""
    };
    char record[PM1_MAX_RECORD_LENGTH];
    char shortRecord[16];

    TEST_ASSERT_EQUAL_UINT(
        0, pm1FormatRecord(shortRecord, sizeof(shortRecord), 1, 2, 0x03, fields));
    TEST_ASSERT_EQUAL_UINT(
        0, pm1FormatRecord(record, sizeof(record), 1, 2, 0x08, fields));
}

} // namespace

void setUp(){}
void tearDown(){}

int main(){
    UNITY_BEGIN();
    RUN_TEST(testCrc16CcittFalse);
    RUN_TEST(testFormatsAuthoritativeRecord);
    RUN_TEST(testRejectsInvalidDestinationAndMask);
    return UNITY_END();
}
