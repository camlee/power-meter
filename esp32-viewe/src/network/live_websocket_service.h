#pragma once

#include <cstdint>

// A bounded binary live-data websocket service. It intentionally has no
// filesystem, LVGL, or settings write access. Port 81 is a temporary
// compatibility boundary while the existing authenticated Arduino WebServer
// remains on port 80 for OTA uploads.
namespace live_websocket_service {

bool begin();
void update();
uint8_t clientCount();
uint8_t clientLimit();

} // namespace live_websocket_service
