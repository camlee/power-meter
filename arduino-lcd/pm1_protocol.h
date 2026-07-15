#ifndef POWER_METER_PM1_PROTOCOL_HEADER
#define POWER_METER_PM1_PROTOCOL_HEADER

#include <stddef.h>
#include <stdint.h>

#define PM1_FIELD_COUNT 9
#define PM1_MAX_RECORD_LENGTH 160

uint16_t pm1Crc16CcittFalse(const char *data, size_t length);

// Each entry in fields is already formatted text. Absent/optional values are
// represented by an empty string. Returns the record length, including LF, or
// zero when the destination is too small.
size_t pm1FormatRecord(char *destination,
                       size_t capacity,
                       uint32_t sequence,
                       uint32_t sourceMillis,
                       uint8_t mask,
                       const char *const fields[PM1_FIELD_COUNT]);

#endif
