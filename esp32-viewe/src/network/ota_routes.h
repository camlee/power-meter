#pragma once

class WebServer;

namespace ota_service {

// Internal route composition hook used by web_server.
void registerRoutes(WebServer& server);

} // namespace ota_service
