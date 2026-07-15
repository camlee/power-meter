#include "pm1_protocol.h"

#include <stdio.h>
#include <string.h>

uint16_t pm1Crc16CcittFalse(const char *data, size_t length){
    uint16_t crc = 0xFFFF;

    for (size_t i = 0; i < length; ++i){
        crc ^= static_cast<uint16_t>(static_cast<uint8_t>(data[i])) << 8;
        for (uint8_t bit = 0; bit < 8; ++bit){
            crc = (crc & 0x8000) ? static_cast<uint16_t>((crc << 1) ^ 0x1021)
                                 : static_cast<uint16_t>(crc << 1);
        }
    }

    return crc;
}

size_t pm1FormatRecord(char *destination,
                       size_t capacity,
                       uint32_t sequence,
                       uint32_t sourceMillis,
                       uint8_t mask,
                       const char *const fields[PM1_FIELD_COUNT]){
    if (destination == NULL || fields == NULL || capacity == 0 || mask > 0x07){
        return 0;
    }

    int written = snprintf(destination,
                           capacity,
                           "PM1,%lu,%lu,%02X",
                           static_cast<unsigned long>(sequence),
                           static_cast<unsigned long>(sourceMillis),
                           static_cast<unsigned int>(mask));
    if (written < 0 || static_cast<size_t>(written) >= capacity){
        return 0;
    }

    size_t length = static_cast<size_t>(written);
    for (uint8_t i = 0; i < PM1_FIELD_COUNT; ++i){
        const char *field = fields[i] == NULL ? "" : fields[i];
        const size_t fieldLength = strlen(field);
        if (length + 1 + fieldLength >= capacity){
            return 0;
        }
        destination[length++] = ',';
        memcpy(destination + length, field, fieldLength);
        length += fieldLength;
        destination[length] = '\0';
    }

    const uint16_t crc = pm1Crc16CcittFalse(destination, length);
    written = snprintf(destination + length,
                       capacity - length,
                       "*%04X\n",
                       static_cast<unsigned int>(crc));
    if (written != 6 || length + static_cast<size_t>(written) >= capacity){
        return 0;
    }

    return length + static_cast<size_t>(written);
}
