#pragma once

#include <cstddef>
#include <cstdint>

#include "data/historical_storage.h"

namespace history_response_encoder {

constexpr size_t kHeaderBytes = 32;
constexpr size_t kRecordBytes = 80;

void encodeHeader(uint8_t (&output)[kHeaderBytes], uint32_t jobId, size_t count,
                  const historical_storage::QueryStatus& status);
void encodeRecord(uint8_t (&output)[kRecordBytes],
                  const historical_storage::PowerBucket& bucket);

} // namespace history_response_encoder
