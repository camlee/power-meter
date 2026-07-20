#include "web_server.h"

#include <WebServer.h>

#include "network/display_web_api.h"
#include "network/ota_routes.h"
#include "network/web_api.h"

namespace web_server {
namespace {

WebServer server(80);
bool running = false;

} // namespace

bool begin() {
    if (running) return true;
    const char* headers[] = {"X-OTA-Token", "If-None-Match"};
    server.collectHeaders(headers, 2);
    ota_service::registerRoutes(server);
    web_api::registerRoutes(server);
    display_web_api::registerRoutes(server);
    server.begin();
    running = true;
    return true;
}

void update() {
    if (running) server.handleClient();
}

bool isRunning() { return running; }

} // namespace web_server
