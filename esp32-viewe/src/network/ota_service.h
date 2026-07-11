#pragma once

// Manual, LAN-only OTA HTTP service. Networking code owns when it is started;
// this module does not connect Wi-Fi or start an access point itself.
namespace ota_service {

// Starts HTTP endpoints on port 80. Safe to call repeatedly.
void begin();

// Must be called regularly from the main event loop after begin().
void update();

bool isRunning();

} // namespace ota_service
