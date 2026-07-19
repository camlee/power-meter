#pragma once

#include <stddef.h>
#include <stdint.h>

#include "network_policy.h"

namespace network_manager {

struct RecoveryCandidatePolicy {
    bool visible;
    bool attempted;
    bool suppressed;
    uint8_t savedOrder;
    int rssi;
};

// Saved order is most-recently-successful first. RSSI only breaks ties, which
// keeps selection predictable instead of roaming to whichever AP is strongest.
inline int chooseRecoveryCandidate(const RecoveryCandidatePolicy* candidates,
                                   size_t count) {
    int selected = -1;
    for (size_t i = 0; i < count; ++i) {
        const RecoveryCandidatePolicy& candidate = candidates[i];
        if (!candidate.visible || candidate.attempted || candidate.suppressed) continue;
        if (selected < 0 ||
            candidate.savedOrder < candidates[selected].savedOrder ||
            (candidate.savedOrder == candidates[selected].savedOrder &&
             candidate.rssi > candidates[selected].rssi)) {
            selected = static_cast<int>(i);
        }
    }
    return selected;
}

constexpr bool failureSuppressesAutomaticCandidate(ConnectionFailure failure) {
    return failure == ConnectionFailure::AuthenticationFailed ||
           failure == ConnectionFailure::IncompatibleSecurity;
}

constexpr uint32_t nextDiscoveryDelay(uint32_t currentDelayMs,
                                      uint32_t maximumDelayMs) {
    return nextReconnectDelay(currentDelayMs, maximumDelayMs);
}

} // namespace network_manager
