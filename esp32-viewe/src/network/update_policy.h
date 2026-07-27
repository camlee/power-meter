#pragma once

#include <stdint.h>

namespace update_policy {

constexpr uint32_t kCheckIntervalSeconds = 24U * 60U * 60U;

constexpr uint32_t retryDelayMs(uint8_t consecutiveFailures) {
    return consecutiveFailures <= 1 ? 5U * 60U * 1000U :
           consecutiveFailures == 2 ? 15U * 60U * 1000U :
           consecutiveFailures == 3 ? 60U * 60U * 1000U :
                                      6U * 60U * 60U * 1000U;
}

constexpr bool automaticCheckDue(bool hasLastCheck, int64_t lastCheckUnixSeconds,
                                 int64_t nowUnixSeconds) {
    if (!hasLastCheck) return true;
    if (nowUnixSeconds < lastCheckUnixSeconds) return true;
    return static_cast<uint64_t>(nowUnixSeconds - lastCheckUnixSeconds) >=
           kCheckIntervalSeconds;
}

} // namespace update_policy
