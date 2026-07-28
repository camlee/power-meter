#include "status_display.h"

#include <Arduino.h>

#include "device/hardware_profile.h"

#if POWER_METER_HAS_STATUS_DISPLAY
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Preferences.h>

#include "device/i2c_bus.h"
#include "network/live_websocket_service.h"
#include "network/network_manager.h"
#include "sensors/sensor_mode.h"
#include "sensors/sensors.h"

#ifndef POWER_METER_SSD1306_ADDRESS
#define POWER_METER_SSD1306_ADDRESS 0x3c
#endif

namespace status_display {
namespace {

constexpr uint8_t kWidth = 128;
constexpr uint8_t kHeight = 64;
constexpr uint32_t kRefreshMs = 1000;
constexpr uint32_t kBeginRetryMs = 5000;
Adafruit_SSD1306 display(kWidth, kHeight, &Wire, -1);
bool ready = false;
uint32_t lastRefreshMs = 0;
uint32_t lastBeginAttemptMs = 0;
bool beginAttempted = false;
Mode selectedMode = Mode::Summary;

const char* networkState() {
    switch (network_manager::getState()) {
        case network_manager::NetworkState::ConnectingSta: return "Connecting";
        case network_manager::NetworkState::ConnectedStaLocal: return "WiFi local";
        case network_manager::NetworkState::ConnectedStaInternet: return "WiFi internet";
        case network_manager::NetworkState::Disconnected:
            return network_manager::isApEnabled() ? "Access point" : "Disconnected";
    }
    return "Network unknown";
}

void printReading(const char* label, sensors::SensorId id) {
    sensors::Reading reading{};
    if (!sensors::getLatest(id, reading) || !sensors::isConfigured(reading)) {
        display.printf("%-3s --\n", label);
        return;
    }
    if (reading.state != sensors::ReadingState::Valid &&
        reading.state != sensors::ReadingState::OutOfRange) {
        display.printf("%-3s %s\n", label,
                       reading.state == sensors::ReadingState::Waiting ? "waiting" : "invalid");
        return;
    }
    display.printf("%-3s %5.1fV %5.1fW\n", label, reading.voltage, reading.power);
}

void drawDense() {
    display.setFont(nullptr);
    display.setTextSize(1);
    display.clearDisplay();
    display.setCursor(0, 0);
    display.printf("%s.local\n", network_manager::getHostname());
    display.printf("%s\n", networkState());
    const char* ssid = network_manager::getCurrentSsid();
    if (ssid && ssid[0]) display.printf("%.21s\n", ssid);
    else if (network_manager::isApEnabled()) display.printf("AP %.18s\n", "active");
    else display.println("No network");
    const bool stationConnected =
        network_manager::getState() == network_manager::NetworkState::ConnectedStaLocal ||
        network_manager::getState() == network_manager::NetworkState::ConnectedStaInternet;
    display.printf("%s\n", stationConnected ? network_manager::getStaIpAddress() :
                   (network_manager::isApEnabled() ? network_manager::getApIpAddress() : "0.0.0.0"));
    printReading("Sol", sensors::SENSOR_IN);
    printReading("Load", sensors::SENSOR_OUT);
    display.printf("%s  WS:%u\n", sensor_mode::label(),
                   static_cast<unsigned>(live_websocket_service::clientCount()));
}

bool readingPower(sensors::SensorId id, float& power) {
    sensors::Reading reading{};
    if (!sensors::getLatest(id, reading) || !sensors::isConfigured(reading) ||
        (reading.state != sensors::ReadingState::Valid &&
         reading.state != sensors::ReadingState::OutOfRange)) return false;
    power = reading.power;
    return isfinite(power);
}

void printSummaryPower(const char* label, sensors::SensorId id) {
    float power = NAN;
    char value[16];
    if (readingPower(id, power)) snprintf(value, sizeof(value), "%.0f W", power);
    else snprintf(value, sizeof(value), "-- W");

    // FreeSans is proportional, so padding with spaces cannot align these
    // values. Draw the label at the left edge and position the complete value
    // string from its measured right pixel bound instead.
    const int16_t baseline = display.getCursorY();
    display.print(label);
    int16_t x1 = 0;
    int16_t y1 = 0;
    uint16_t width = 0;
    uint16_t height = 0;
    display.getTextBounds(value, 0, baseline, &x1, &y1, &width, &height);
    display.setCursor(static_cast<int16_t>(kWidth) - x1 - static_cast<int16_t>(width), baseline);
    display.print(value);
}

void drawSummary() {
    display.clearDisplay();
    display.setFont(&FreeSans9pt7b);
    display.setTextSize(1);
    display.setCursor(0, 15);
    const bool stationConnected =
        network_manager::getState() == network_manager::NetworkState::ConnectedStaLocal ||
        network_manager::getState() == network_manager::NetworkState::ConnectedStaInternet;
    switch ((millis() / 4000) % 3U) {
        case 0:
            display.printf("%s.local", network_manager::getHostname());
            break;
        case 1: {
            const char* ssid = network_manager::getCurrentSsid();
            if (stationConnected && ssid && ssid[0]) display.printf("WiFi %.10s", ssid);
            else display.printf("%.16s", networkState());
            break;
        }
        default:
            if (stationConnected) display.print(network_manager::getStaIpAddress());
            else if (network_manager::isApEnabled()) display.print(network_manager::getApIpAddress());
            else display.printf("%.16s", networkState());
            break;
    }

    display.setCursor(0, 36);
    printSummaryPower("SOLAR", sensors::SENSOR_IN);
    display.setCursor(0, 57);
    printSummaryPower("LOAD", sensors::SENSOR_OUT);
}

void draw() {
    if (selectedMode == Mode::Dense) drawDense();
    else drawSummary();
    display.display();
}

} // namespace

bool begin() {
    if (ready) return true;
    beginAttempted = true;
    lastBeginAttemptMs = millis();
    if (!hardware_profile::kHasStatusDisplay || !i2c_bus::isReady()) return false;
    i2c_bus::Guard guard;
    if (!guard) return false;
    // periphBegin=false preserves the pins/frequency owned by i2c_bus.
    if (!display.begin(SSD1306_SWITCHCAPVCC, POWER_METER_SSD1306_ADDRESS,
                       true, false)) {
        Serial.println("status_display: SSD1306 initialization failed");
        return false;
    }
    display.setRotation(2);
    display.setTextColor(SSD1306_WHITE);
    display.setTextWrap(false);
    Preferences preferences;
    if (preferences.begin("status_oled", true)) {
        selectedMode = preferences.getBool("dense", false) ? Mode::Dense : Mode::Summary;
        preferences.end();
    }
    display.setFont(nullptr);
    display.setTextSize(1);
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println("Power meter");
    display.println("Starting...");
    display.display();
    ready = true;
    Serial.printf("status_display: ready address=0x%02x\n", POWER_METER_SSD1306_ADDRESS);
    return true;
}

void update() {
    const uint32_t now = millis();
    if (!ready) {
        if (i2c_bus::isReady() &&
            (!beginAttempted || now - lastBeginAttemptMs >= kBeginRetryMs)) begin();
        return;
    }
    if (now - lastRefreshMs < kRefreshMs) return;
    lastRefreshMs = now;
    i2c_bus::Guard guard(100);
    if (guard) draw();
}

bool isReady() { return ready; }

Mode mode() { return selectedMode; }

const char* modeName() { return selectedMode == Mode::Dense ? "dense" : "summary"; }

bool setMode(Mode requestedMode) {
    Preferences preferences;
    if (!preferences.begin("status_oled", false)) return false;
    const bool dense = requestedMode == Mode::Dense;
    const bool saved = preferences.putBool("dense", dense) == sizeof(uint8_t);
    preferences.end();
    if (saved) selectedMode = requestedMode;
    return saved;
}

} // namespace status_display

#else

namespace status_display {
bool begin() { return false; }
void update() {}
bool isReady() { return false; }
Mode mode() { return Mode::Summary; }
const char* modeName() { return "summary"; }
bool setMode(Mode requestedMode) { return requestedMode == Mode::Summary; }
} // namespace status_display

#endif
