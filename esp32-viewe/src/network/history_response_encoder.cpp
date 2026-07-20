#include "history_response_encoder.h"

#include <cstring>

namespace history_response_encoder {
namespace {

void putLe16(uint8_t* dest, uint16_t value) {
    dest[0] = value & 0xff;
    dest[1] = value >> 8;
}

void putLe32(uint8_t* dest, uint32_t value) {
    dest[0] = value & 0xff;
    dest[1] = (value >> 8) & 0xff;
    dest[2] = (value >> 16) & 0xff;
    dest[3] = (value >> 24) & 0xff;
}

void putLeFloat(uint8_t* dest, float value) {
    uint32_t bits = 0;
    memcpy(&bits, &value, sizeof(bits));
    putLe32(dest, bits);
}

void putLeDouble(uint8_t* dest, double value) {
    uint64_t bits = 0;
    memcpy(&bits, &value, sizeof(bits));
    for (uint8_t i = 0; i < 8; ++i) dest[i] = static_cast<uint8_t>(bits >> (i * 8));
}

} // namespace

void encodeHeader(uint8_t (&output)[kHeaderBytes], uint32_t jobId, size_t count,
                  const historical_storage::QueryStatus& status) {
    memset(output, 0, sizeof(output));
    putLe32(output, 0x32485056); // "VPH2"; VPH1 was the incompatible alpha layout.
    output[4] = 2;
    output[5] = 2;
    const uint16_t flags = (status.incomplete ? 1 : 0) | (status.hasInferredTime ? 2 : 0);
    putLe16(output + 6, flags);
    putLe16(output + 8, static_cast<uint16_t>(count));
    putLe16(output + 10, kRecordBytes);
    putLe32(output + 12, jobId);
    putLeDouble(output + 16, static_cast<double>(status.startUnixMs));
    putLeDouble(output + 24, static_cast<double>(status.endUnixMs));
}

void encodeRecord(uint8_t (&output)[kRecordBytes],
                  const historical_storage::PowerBucket& bucket) {
    memset(output, 0, sizeof(output));
    putLeDouble(output, static_cast<double>(bucket.startUnixMs));
    putLe32(output + 8, bucket.coveredMs);
    output[12] = bucket.configuredChannelMask;
    output[13] = bucket.timeFlags;
    output[14] = bucket.qualityFlags;
    for (size_t value = 0; value < historical_storage::kSensorCount; ++value) {
        putLeFloat(output + 16 + value * 4, bucket.energyWh[value]);
        putLe32(output + 48 + value * 4, bucket.channelCoverageMs[value]);
    }
    for (size_t value = 0; value < historical_storage::COMPONENT_COUNT; ++value) {
        putLeFloat(output + 28 + value * 4, bucket.componentEnergyWh[value]);
        putLe32(output + 60 + value * 4, bucket.componentCoverageMs[value]);
    }
}

} // namespace history_response_encoder
