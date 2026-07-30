#pragma once

#include <stdint.h>

// A cheap cross-surface invalidation token.  The LVGL screens retain their
// existing ownership of widgets; the browser receives this revision in every
// live frame and refreshes its read model when it changes.
namespace device_state {

enum class Domain : uint8_t {
    DeviceIdentity,
    Network,
    Calibration,
    SensorMapping,
    Time,
    History,
    Update,
};

uint32_t revision();
void changed(Domain domain);

} // namespace device_state
