#pragma once

#include <stdint.h>

namespace network_manager {

enum class ConnectionFailure {
    None,
    NetworkNotFound,
    AuthenticationFailed,
    IncompatibleSecurity,
    ConnectionFailed,
    TimedOut,
    LinkLost,
};

constexpr bool connectionFailureNeedsUserAction(ConnectionFailure failure) {
    return failure == ConnectionFailure::AuthenticationFailed ||
           failure == ConnectionFailure::IncompatibleSecurity;
}

constexpr bool connectionFailureShouldRetry(ConnectionFailure failure) {
    return failure != ConnectionFailure::None &&
           !connectionFailureNeedsUserAction(failure);
}

constexpr uint32_t nextReconnectDelay(uint32_t currentDelayMs,
                                      uint32_t maximumDelayMs) {
    if (currentDelayMs >= maximumDelayMs) return maximumDelayMs;
    const uint32_t doubled = currentDelayMs > UINT32_MAX / 2
                                 ? UINT32_MAX
                                 : currentDelayMs * 2;
    return doubled > maximumDelayMs ? maximumDelayMs : doubled;
}

} // namespace network_manager
