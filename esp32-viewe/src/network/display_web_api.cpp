#include "display_web_api.h"

#include <WebServer.h>
#include "device/hardware_profile.h"

#if POWER_METER_HAS_TOUCH_UI
#include <WiFi.h>
#include <cstring>

#include "lvgl_v8_port.h"
#include "memory/heap_policy.h"
#include "network/http_utils.h"
#include "ui/input/remote_input.h"
#include "ui/theme/ui_theme.h"

namespace display_web_api {
namespace {

WebServer* server = nullptr;
constexpr uint32_t kScreenshotMinIntervalMs = 250;
uint32_t lastScreenshotMs = 0;

ui_theme::Mode parseAppearance(const String& value) {
    if (value == "light") return ui_theme::Mode::Light;
    if (value == "dark") return ui_theme::Mode::Dark;
    return ui_theme::Mode::Auto;
}

void screenshot() {
    const uint32_t now = millis();
    if (now - lastScreenshotMs < kScreenshotMinIntervalMs) {
        server->sendHeader("Retry-After", "1");
        server->send(429, "application/json", "{\"error\":\"screenshot rate limited\"}");
        return;
    }
    lastScreenshotMs = now;
    uint16_t width = 0, height = 0;
    const uint8_t* frameBuffer = lvgl_port_get_remote_framebuffer(&width, &height);
    if (!frameBuffer) {
        server->send(503, "application/json", "{\"error\":\"framebuffer unavailable\"}");
        return;
    }

    uint16_t scale = 1;
    if (server->hasArg("scale")) {
        const int requestedScale = server->arg("scale").toInt();
        if (requestedScale == 2 || requestedScale == 4) scale = requestedScale;
    }
    const uint16_t outputWidth = width / scale;
    const uint16_t outputHeight = height / scale;

    if (server->arg("format") == "ppm") {
        const String ppmHeader = String("P6\n") + outputWidth + " " + outputHeight + "\n255\n";
        server->setContentLength(ppmHeader.length() + static_cast<size_t>(outputWidth) * outputHeight * 3);
        server->send(200, "image/x-portable-pixmap", "");
        WiFiClient client = server->client();
        client.setConnectionTimeout(1500);
        if (!http_utils::writeClient(client,
                reinterpret_cast<const uint8_t*>(ppmHeader.c_str()), ppmHeader.length())) return;
        uint8_t line[320 * 3];
        for (uint16_t y = 0; y < outputHeight && client.connected(); ++y) {
            for (uint16_t x = 0; x < outputWidth; ++x) {
                const size_t source = (static_cast<size_t>(y * scale) * width + x * scale) * 2;
#if LV_COLOR_16_SWAP
                const uint16_t rgb565 = (static_cast<uint16_t>(frameBuffer[source]) << 8) | frameBuffer[source + 1];
#else
                const uint16_t rgb565 = frameBuffer[source] | (static_cast<uint16_t>(frameBuffer[source + 1]) << 8);
#endif
                line[x * 3] = ((rgb565 >> 11) & 0x1f) * 255 / 31;
                line[x * 3 + 1] = ((rgb565 >> 5) & 0x3f) * 255 / 63;
                line[x * 3 + 2] = (rgb565 & 0x1f) * 255 / 31;
            }
            if (!http_utils::writeClient(client, line, outputWidth * 3)) return;
        }
        return;
    }

    constexpr size_t kHeaderBytes = 54;
    const uint32_t pixelBytes = static_cast<uint32_t>(outputWidth) * outputHeight * 3;
    const size_t responseBytes = kHeaderBytes + pixelBytes;
    auto* response = static_cast<uint8_t*>(heap_policy::mallocPreferred(responseBytes));
    if (!response) {
        server->send(503, "application/json", "{\"error\":\"screenshot buffer unavailable\"}");
        return;
    }
    memset(response, 0, kHeaderBytes);
    response[0] = 'B'; response[1] = 'M';
    http_utils::putLe32(response + 2, kHeaderBytes + pixelBytes);
    http_utils::putLe32(response + 10, kHeaderBytes);
    http_utils::putLe32(response + 14, 40);
    http_utils::putLe32(response + 18, outputWidth);
    http_utils::putLe32(response + 22, outputHeight);
    http_utils::putLe16(response + 26, 1);
    http_utils::putLe16(response + 28, 24);
    http_utils::putLe32(response + 34, pixelBytes);

    uint8_t* output = response + kHeaderBytes;
    const bool displayLocked = lvgl_port_lock(100);
    for (int outputY = outputHeight - 1; outputY >= 0; --outputY) {
        const uint16_t sourceY = outputY * scale;
        for (size_t x = 0; x < outputWidth; ++x) {
            const size_t source = (static_cast<size_t>(sourceY) * width + x * scale) * 2;
#if LV_COLOR_16_SWAP
            const uint16_t rgb565 = (static_cast<uint16_t>(frameBuffer[source]) << 8) | frameBuffer[source + 1];
#else
            const uint16_t rgb565 = frameBuffer[source] | (static_cast<uint16_t>(frameBuffer[source + 1]) << 8);
#endif
            *output++ = (rgb565 & 0x1f) * 255 / 31;
            *output++ = ((rgb565 >> 5) & 0x3f) * 255 / 63;
            *output++ = ((rgb565 >> 11) & 0x1f) * 255 / 31;
        }
    }
    if (displayLocked) lvgl_port_unlock();

    server->setContentLength(responseBytes);
    server->send(200, "image/bmp", "");
    WiFiClient client = server->client();
    client.setConnectionTimeout(1500);
    http_utils::writeClient(client, response, responseBytes);
    heap_caps_free(response);
}

bool parseRemotePoint(uint16_t& x, uint16_t& y, bool* pressed = nullptr) {
    uint32_t parsedX = 0, parsedY = 0;
    const String body = server->arg("plain");
    if (!http_utils::jsonUnsigned(body, "x", parsedX) ||
        !http_utils::jsonUnsigned(body, "y", parsedY) || parsedX > 319 || parsedY > 479) {
        server->send(400, "application/json", "{\"error\":\"x and y must be display coordinates\"}");
        return false;
    }
    if (pressed && !http_utils::jsonBool(body, "pressed", *pressed)) {
        server->send(400, "application/json", "{\"error\":\"pressed must be boolean\"}");
        return false;
    }
    x = parsedX;
    y = parsedY;
    return true;
}

void remoteTap() {
    uint16_t x, y;
    if (!parseRemotePoint(x, y)) return;
    remote_input::tap(x, y);
    server->send(200, "application/json", "{\"ok\":true}");
}

void remotePointer() {
    uint16_t x, y;
    bool pressed = false;
    if (!parseRemotePoint(x, y, &pressed)) return;
    remote_input::setPointer(x, y, pressed);
    server->send(200, "application/json", "{\"ok\":true}");
}

} // namespace

void registerRoutes(WebServer& value) {
    server = &value;
    server->on("/api/v1/display/screenshot.bmp", HTTP_GET, screenshot);
    server->on("/api/v1/display/tap", HTTP_POST, remoteTap);
    server->on("/api/v1/display/pointer", HTTP_POST, remotePointer);
}

const char* appearanceModeName() {
    switch (ui_theme::mode()) {
        case ui_theme::Mode::Light: return "light";
        case ui_theme::Mode::Dark: return "dark";
        case ui_theme::Mode::Auto: return "auto";
    }
    return "auto";
}

bool isDark() { return ui_theme::isDark(); }

bool isValidAppearance(const String& value) {
    return value == "light" || value == "dark" || value == "auto";
}

void setAppearance(const String& value) {
    const ui_theme::Mode requested = parseAppearance(value);
    if (requested != ui_theme::mode()) ui_theme::setMode(requested);
}

String lvglVersion() {
    return String(LVGL_VERSION_MAJOR) + "." + LVGL_VERSION_MINOR + "." + LVGL_VERSION_PATCH;
}

} // namespace display_web_api
#else
namespace display_web_api {

void registerRoutes(WebServer&) {}
const char* appearanceModeName() { return "auto"; }
bool isDark() { return false; }
bool isValidAppearance(const String& value) {
    return value == "light" || value == "dark" || value == "auto";
}
void setAppearance(const String&) {}
String lvglVersion() { return String(); }

} // namespace display_web_api
#endif
