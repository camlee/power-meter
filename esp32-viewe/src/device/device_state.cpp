#include "device_state.h"

#include <atomic>

namespace device_state {
namespace {
std::atomic<uint32_t> stateRevision{1};
}

uint32_t revision() { return stateRevision.load(std::memory_order_relaxed); }

void changed(Domain) {
    // The domain is intentionally retained at the call site for future
    // websocket change events. A monotonic revision is enough for the first
    // browser client: it simply refreshes the small status document.
    stateRevision.fetch_add(1, std::memory_order_relaxed);
}

} // namespace device_state
