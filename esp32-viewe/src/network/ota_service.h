#pragma once

#include <cstdint>

// Manual, LAN-only OTA HTTP service. Networking code owns when it is started;
// this module does not connect Wi-Fi or start an access point itself.
namespace ota_service {

// Starts HTTP endpoints on port 80. Safe to call repeatedly.
void begin();

// Must be called regularly from the main event loop after begin().
void update();

bool isRunning();

// Call after application setup has completed. A pending OTA image is only
// confirmed after this point and a short period of normal main-loop service.
void setApplicationReady();

// Called from the main loop to prove the application is still servicing its
// normal work while a newly booted image is awaiting confirmation.
void noteHealthyLoop();

// Small diagnostic helpers used by the on-device Debug screen.
const char* healthStatus();
const char* runningPartitionLabel();
const char* bootPartitionLabel();
const char* runningImageState();
bool rollbackDetected();
bool rollbackSupported();
uint32_t validationRemainingMs();

} // namespace ota_service
