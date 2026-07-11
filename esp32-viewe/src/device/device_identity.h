#pragma once

#include <stddef.h>

// Stable identity for one physical meter.  The editable device ID is used as
// its hostname; the hardware ID remains tied to the ESP32 eFuse MAC.
namespace device_identity {

void init();

const char* getDeviceId();
const char* getHostname();
const char* getHardwareId();

// Accepts DNS-safe IDs (lowercase letters, numbers, and hyphens).  The value
// is persisted in NVS and takes effect after services are restarted/rebooted.
bool setDeviceId(const char* deviceId);

} // namespace device_identity
