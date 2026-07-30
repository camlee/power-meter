#include "web_api.h"

#include <Arduino.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <Preferences.h>
#include <WebServer.h>
#include <esp_system.h>
#include <esp_heap_caps.h>
#include <math.h>
#include <time.h>

#include "build_time.h"
#include "data/historical_storage.h"
#include "data/energy_cycle.h"
#include "data/history_query_service.h"
#include "device/device_identity.h"
#include "device/status_display.h"
#include "device/hardware_profile.h"
#include "device/device_state.h"
#include "memory/heap_policy.h"
#include "network/display_web_api.h"
#include "network/history_response_encoder.h"
#include "network/http_utils.h"
#include "network/internet_update_service.h"
#include "network/network_manager.h"
#include "network/live_websocket_service.h"
#include "network/ota_service.h"
#include "network/web_assets.generated.h"
#include "sensors/pm1_uart_protocol.h"
#include "sensors/sensor_calibration.h"
#include "sensors/sensor_mapping.h"
#include "sensors/sensor_mode.h"
#include "sensors/sensor_source_ads1115.h"
#include "sensors/sensor_source_uart.h"
#include "sensors/sensors.h"
#include "time/time_service.h"

#if POWER_METER_HAS_TOUCH_UI
#include "lvgl_v8_port.h"
#endif

namespace web_api {
namespace {

WebServer* server = nullptr;

const char* resetReasonName(esp_reset_reason_t reason) {
    switch (reason) {
        case ESP_RST_POWERON: return "power_on";
        case ESP_RST_SW: return "software";
        case ESP_RST_PANIC: return "panic";
        case ESP_RST_INT_WDT: return "interrupt_watchdog";
        case ESP_RST_TASK_WDT: return "task_watchdog";
        case ESP_RST_WDT: return "watchdog";
        case ESP_RST_BROWNOUT: return "brownout";
        default: return "other";
    }
}

const char* sensorModeName(sensor_mode::Mode mode) {
    return sensor_mode::name(mode);
}

void serveWebAsset() {
    const String requestPath = server->uri();
    const web_assets::Asset* selected = nullptr;
    for (size_t i = 0; i < web_assets::kAssetCount; ++i) {
        const auto& asset = web_assets::kAssets[i];
        if (requestPath == asset.path || (requestPath == "/" && strcmp(asset.path, "/index.html") == 0)) {
            selected = &asset;
            break;
        }
    }
    // Hash routes are intentionally served by the SPA shell. API requests
    // never reach this fallback, which keeps a miss cheap and unambiguous.
    if (!selected && !requestPath.startsWith("/api/")) {
        for (size_t i = 0; i < web_assets::kAssetCount; ++i) {
            if (strcmp(web_assets::kAssets[i].path, "/index.html") == 0) {
                selected = &web_assets::kAssets[i];
                break;
            }
        }
    }
    if (!selected) { server->send(404, "application/json", "{\"error\":\"not found\"}"); return; }

    const String etag = String("\"") + selected->etag + "\"";
    server->sendHeader("ETag", etag);
    server->sendHeader("Content-Encoding", "gzip");
    server->sendHeader("Cache-Control", selected->immutable
        ? "public, max-age=31536000, immutable"
        : "no-cache, max-age=0, must-revalidate");
    if (server->header("If-None-Match") == etag) {
        server->send(304, selected->contentType, "");
        return;
    }
    server->send_P(200, selected->contentType,
                  reinterpret_cast<PGM_P>(const_cast<uint8_t*>(selected->data)), selected->size);
}

void webStatus() {
    sensors::Reading in{}, out{}, aux{};
    const bool hasIn = sensors::getLatest(sensors::SENSOR_SOLAR, in);
    const bool hasOut = sensors::getLatest(sensors::SENSOR_LOAD, out);
    sensors::getLatest(sensors::SENSOR_BATTERY, aux);
    float net = NAN;
    sensors::getNetBatteryPower(net);
    time_service::Anchor anchor{};
    const char* timeSource = time_service::getCurrentAnchor(anchor)
        ? time_service::sourceName(anchor.source) : "unanchored";
    auto jsonFloat = [](char* dest, size_t size, float value) {
        if (!isfinite(value)) snprintf(dest, size, "null");
        else snprintf(dest, size, "%.4g", value);
    };
    char inVoltage[16], inCurrent[16], inPower[16], outVoltage[16], outCurrent[16], outPower[16], auxPower[16], netPower[16];
    const bool eligibleIn = hasIn && sensors::isCalculationEligible(in);
    const bool eligibleOut = hasOut && sensors::isCalculationEligible(out);
    const bool eligibleAux = sensors::isCalculationEligible(aux);
    jsonFloat(inVoltage, sizeof(inVoltage), eligibleIn ? in.voltage : NAN);
    jsonFloat(inCurrent, sizeof(inCurrent), eligibleIn ? in.current : NAN);
    jsonFloat(inPower, sizeof(inPower), eligibleIn ? in.power : NAN);
    jsonFloat(outVoltage, sizeof(outVoltage), eligibleOut ? out.voltage : NAN);
    jsonFloat(outCurrent, sizeof(outCurrent), eligibleOut ? out.current : NAN);
    jsonFloat(outPower, sizeof(outPower), eligibleOut ? out.power : NAN);
    jsonFloat(auxPower, sizeof(auxPower), eligibleAux ? aux.power : NAN);
    jsonFloat(netPower, sizeof(netPower), net);
    char date[24] = "-", clock[24] = "-";
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
    if (timeinfo.tm_year > 70) {
        strftime(date, sizeof(date), "%b %e %Y", &timeinfo);
        strftime(clock, sizeof(clock), "%I:%M:%S %p", &timeinfo);
    }
    const size_t storageTotal = LittleFS.totalBytes();
    const unsigned storagePercent = storageTotal
        ? static_cast<unsigned>((LittleFS.usedBytes() * 100U) / storageTotal) : 0;
    char response[1664];
    snprintf(response, sizeof(response),
             "{\"api_version\":1,\"web_build\":\"%s\",\"state_revision\":%lu,"
             "\"hardware_profile\":\"%s\",\"capabilities\":{\"touch_display\":%s,"
             "\"status_display\":%s,\"controller_mode\":\"%s\",\"pwm_ui\":%s,"
             "\"sensor_modes\":{\"adc\":%s,\"ads1115\":%s,\"uart\":%s,\"demo\":true}},"
             "\"device_id\":\"%s\",\"hostname\":\"%s\",\"uptime_ms\":%lu,"
             "\"time_source\":\"%s\",\"date\":\"%s\",\"time\":\"%s\","
             "\"build_version\":\"%s\",\"build_date\":\"%s\",\"build_time\":\"%s\","
             "\"sensor_mode\":\"%s\",\"appearance\":{\"mode\":\"%s\",\"dark\":%s},"
             "\"data_storage_percent\":%u,\"ws_connections\":%u,\"ws_connection_limit\":%u,"
             "\"in\":{\"voltage\":%s,\"current\":%s,\"power\":%s},"
             "\"out\":{\"voltage\":%s,\"current\":%s,\"power\":%s},"
             "\"aux\":{\"power\":%s},\"net_battery_power\":%s,"
             "\"network\":{\"state\":%u,\"station_ssid\":",
             web_assets::kBuildId, static_cast<unsigned long>(device_state::revision()),
             hardware_profile::kName, hardware_profile::kHasTouchUi ? "true" : "false",
             hardware_profile::kHasStatusDisplay ? "true" : "false",
             hardware_profile::kControllerMode,
             hardware_profile::kControllerIsPwm ? "true" : "false",
             hardware_profile::kHasEsp32Adc ? "true" : "false",
             hardware_profile::kHasAds1115 ? "true" : "false",
             hardware_profile::kSupportsUart ? "true" : "false",
             device_identity::getDeviceId(), network_manager::getHostname(), static_cast<unsigned long>(millis()),
             timeSource, date, clock, BUILD_VERSION, BUILD_DATE, BUILD_TIME,
             sensorModeName(sensor_mode::get()), display_web_api::appearanceModeName(),
             display_web_api::isDark() ? "true" : "false", storagePercent,
             static_cast<unsigned>(live_websocket_service::clientCount()),
             static_cast<unsigned>(live_websocket_service::clientLimit()),
             inVoltage, inCurrent, inPower, outVoltage, outCurrent, outPower,
             auxPower, netPower, static_cast<unsigned>(network_manager::getState()));
    String responseJson(response);
    http_utils::appendJsonString(responseJson, network_manager::getCurrentSsid());
    responseJson += ",\"station_ip\":";
    http_utils::appendJsonString(responseJson, network_manager::getStaIpAddress());
    responseJson += ",\"ap_ssid\":";
    http_utils::appendJsonString(responseJson, network_manager::getCurrentApSsid());
    responseJson += ",\"ap_ip\":";
    http_utils::appendJsonString(responseJson,
        network_manager::isApEnabled() ? network_manager::getApIpAddress() : "Off");
    responseJson += "}}";
    server->sendHeader("Cache-Control", "no-store");
    server->send(200, "application/json", responseJson);
}

const char* readingStateName(sensors::ReadingState state) {
    switch (state) {
        case sensors::ReadingState::NotConfigured: return "not_configured";
        case sensors::ReadingState::Waiting: return "waiting";
        case sensors::ReadingState::Valid: return "valid";
        case sensors::ReadingState::OutOfRange: return "out_of_range";
        case sensors::ReadingState::Invalid: return "invalid";
        case sensors::ReadingState::Stale: return "stale";
    }
    return "invalid";
}

const char* dutyStateName(sensors::DutyState state) {
    switch (state) {
        case sensors::DutyState::NotReported: return "not_reported";
        case sensors::DutyState::Valid: return "valid";
        case sensors::DutyState::Invalid: return "invalid";
    }
    return "invalid";
}

void appendJsonFloat(String& json, float value) {
    if (!isfinite(value)) json += "null";
    else json += String(value, 4);
}

void writeU16(uint8_t* destination, uint16_t value) {
    destination[0] = static_cast<uint8_t>(value);
    destination[1] = static_cast<uint8_t>(value >> 8);
}

void writeU32(uint8_t* destination, uint32_t value) {
    destination[0] = static_cast<uint8_t>(value);
    destination[1] = static_cast<uint8_t>(value >> 8);
    destination[2] = static_cast<uint8_t>(value >> 16);
    destination[3] = static_cast<uint8_t>(value >> 24);
}

void writeF32(uint8_t* destination, float value) {
    uint32_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value));
    memcpy(&bits, &value, sizeof(bits));
    writeU32(destination, bits);
}

constexpr uint32_t kAdcCaptureMagic = 0x31434441; // "ADC1" little endian
constexpr size_t kAdcCaptureHeaderBytes = 32;
constexpr size_t kAdcCaptureWindowBytes = 32;
constexpr size_t kAdcCapturePointBytes = 16;

const char* adcCaptureChannelName(sensors::SensorId channel) {
    switch (channel) {
        case sensors::SENSOR_SOLAR: return "in";
        case sensors::SENSOR_LOAD: return "out";
        case sensors::SENSOR_BATTERY: return "aux";
        default: return "in";
    }
}

bool parseAdcCaptureChannel(const String& value, sensors::SensorId& channel) {
    if (value == "in") channel = sensors::SENSOR_SOLAR;
    else if (value == "out") channel = sensors::SENSOR_LOAD;
    else if (value == "aux") channel = sensors::SENSOR_BATTERY;
    else return false;
    return true;
}

bool parseAdcCaptureId(uint32_t& captureId) {
    if (!server->hasArg("id")) return false;
    const String value = server->arg("id");
    if (value.isEmpty()) return false;
    uint32_t parsed = 0;
    for (size_t i = 0; i < value.length(); ++i) {
        const char digit = value[i];
        if (digit < '0' || digit > '9') return false;
        const uint8_t number = static_cast<uint8_t>(digit - '0');
        if (parsed > (UINT32_MAX - number) / 10U) return false;
        parsed = parsed * 10U + number;
    }
    if (parsed == 0) return false;
    captureId = parsed;
    return true;
}

void encodeAdcCaptureHeader(uint8_t* out, const sensors::AdcCaptureResult& capture) {
    memset(out, 0, kAdcCaptureHeaderBytes);
    writeU32(out, kAdcCaptureMagic);
    out[4] = 1;
    out[5] = 1;
    writeU16(out + 6, kAdcCaptureHeaderBytes +
        capture.windowCount * kAdcCaptureWindowBytes);
    writeU16(out + 8, capture.pointCount);
    out[10] = capture.windowCount;
    out[11] = sensors::kAdcCaptureWindowCount;
    writeU32(out + 12, capture.requestedIntervalUs);
    writeU32(out + 16, capture.measuredIntervalUs);
    writeU16(out + 20, capture.droppedPoints);
    out[22] = static_cast<uint8_t>(capture.channel);
    const uint32_t durationUs = capture.windowCount
        ? capture.windows[capture.windowCount - 1].endUs : 0;
    writeU32(out + 24, durationUs);
    writeU16(out + 28, kAdcCapturePointBytes);
    writeU16(out + 30, kAdcCaptureWindowBytes);
}

void encodeAdcCaptureWindow(uint8_t* out, const sensors::AdcCaptureWindow& window) {
    memset(out, 0, kAdcCaptureWindowBytes);
    writeU32(out, window.startUs);
    writeU32(out + 4, window.endUs);
    writeU16(out + 8, window.firstPoint);
    writeU16(out + 10, window.pointCount);
    out[12] = static_cast<uint8_t>(window.reading.state);
    out[13] = window.reading.configured ? 1 : 0;
    out[14] = static_cast<uint8_t>(window.reading.dutyState);
    writeF32(out + 16, window.reading.voltage);
    writeF32(out + 20, window.reading.current);
    writeF32(out + 24, window.reading.power);
    writeF32(out + 28, window.reading.dutyState == sensors::DutyState::Valid
        ? window.reading.dutyCycle : NAN);
}

void encodeAdcCapturePoint(uint8_t* out, const sensors::AdcCapturePoint& point) {
    writeU32(out, point.elapsedUs);
    writeF32(out + 4, point.voltage);
    writeF32(out + 8, point.current);
    writeF32(out + 12, point.power);
}

void webAdcCapture() {
    if (server->method() == HTTP_DELETE) {
        uint32_t captureId = 0;
        if (!parseAdcCaptureId(captureId)) {
            server->send(400, "application/json",
                         "{\"error\":\"A valid capture id is required\"}");
            return;
        }
        if (!sensors::cancelAdcCapture(captureId)) {
            server->send(409, "application/json",
                         "{\"error\":\"Capture id is no longer active\"}");
            return;
        }
        server->sendHeader("Cache-Control", "no-store");
        server->send(200, "application/json", "{\"state\":\"idle\"}");
        return;
    }

    if (server->method() == HTTP_POST) {
        const sensor_mode::Mode mode = sensor_mode::get();
        if (mode != sensor_mode::Mode::Adc && mode != sensor_mode::Mode::Ads1115) {
            server->send(409, "application/json",
                         "{\"error\":\"An ADC sensor mode is not active\"}");
            return;
        }
        String requestedChannel;
        sensors::SensorId channel = sensors::SENSOR_SOLAR;
        if (!http_utils::jsonString(server->arg("plain"), "channel", requestedChannel) ||
            !parseAdcCaptureChannel(requestedChannel, channel)) {
            server->send(400, "application/json",
                         "{\"error\":\"channel must be in, out, or aux\"}");
            return;
        }
        uint32_t captureId = 0;
        if (!sensors::requestAdcCapture(channel, captureId)) {
            const sensors::AdcCaptureStatus status = sensors::getAdcCaptureStatus();
            if (status.state == sensors::AdcCaptureState::Idle) {
                server->send(503, "application/json",
                             "{\"error\":\"Not enough memory to start capture\"}");
            } else {
                server->send(409, "application/json",
                             "{\"error\":\"A capture is already pending or ready\"}");
            }
            return;
        }
        server->sendHeader("Cache-Control", "no-store");
        char response[128];
        snprintf(response, sizeof(response),
                 "{\"state\":\"armed\",\"capture_id\":%lu,\"channel\":\"%s\",\"windows\":3}",
                 static_cast<unsigned long>(captureId), adcCaptureChannelName(channel));
        server->send(202, "application/json", response);
        return;
    }

    const sensors::AdcCaptureStatus status = sensors::getAdcCaptureStatus();
    char response[224];
    snprintf(response, sizeof(response),
             "{\"state\":\"%s\",\"capture_id\":%lu,\"channel\":\"%s\",\"points\":%u,"
             "\"windows\":%u,\"target_windows\":%u,\"dropped_points\":%u}",
             sensors::adcCaptureStateName(status.state),
             static_cast<unsigned long>(status.captureId),
             adcCaptureChannelName(status.channel), status.pointCount,
             status.windowCount, status.targetWindowCount, status.droppedPoints);
    server->sendHeader("Cache-Control", "no-store");
    server->send(200, "application/json", response);
}

void webAdcCaptureData() {
    uint32_t captureId = 0;
    if (!parseAdcCaptureId(captureId)) {
        server->send(400, "application/json",
                     "{\"error\":\"A valid capture id is required\"}");
        return;
    }
    auto* capture = static_cast<sensors::AdcCaptureResult*>(
        heap_policy::mallocPreferred(sizeof(sensors::AdcCaptureResult)));
    if (!capture) {
        server->send(503, "application/json",
                     "{\"error\":\"Not enough memory to transfer capture\"}");
        return;
    }
    if (!sensors::takeAdcCapture(captureId, *capture)) {
        heap_caps_free(capture);
        server->send(409, "application/json",
                     "{\"error\":\"Capture is not ready\"}");
        return;
    }

    const size_t responseBytes = kAdcCaptureHeaderBytes +
        capture->windowCount * kAdcCaptureWindowBytes +
        capture->pointCount * kAdcCapturePointBytes;
    server->sendHeader("Cache-Control", "no-store");
    server->setContentLength(responseBytes);
    server->send(200, "application/vnd.viewe.adc-capture-v1", "");
    WiFiClient client = server->client();
    client.setConnectionTimeout(1500);

    uint8_t encoded[kAdcCaptureWindowBytes];
    encodeAdcCaptureHeader(encoded, *capture);
    bool sent = http_utils::writeClient(client, encoded, kAdcCaptureHeaderBytes);
    for (uint8_t i = 0; sent && i < capture->windowCount; ++i) {
        encodeAdcCaptureWindow(encoded, capture->windows[i]);
        sent = http_utils::writeClient(client, encoded, kAdcCaptureWindowBytes);
    }
    for (uint16_t i = 0; sent && i < capture->pointCount; ++i) {
        encodeAdcCapturePoint(encoded, capture->points[i]);
        sent = http_utils::writeClient(client, encoded, kAdcCapturePointBytes);
    }
    heap_caps_free(capture);
}

bool calibrationSourceForMode(sensor_mode::Mode mode, sensors::calibration::Source& source) {
    if (mode == sensor_mode::Mode::Adc) {
        source = sensors::calibration::Source::Esp32Adc;
        return true;
    }
    if (mode == sensor_mode::Mode::Ads1115) {
        source = sensors::calibration::Source::Ads1115;
        return true;
    }
    return false;
}

void appendCalibrationValue(String& json, sensors::calibration::Source source,
                            uint8_t sensor, sensors::calibration::Measurement measurement) {
    const auto value = sensors::calibration::get(source, sensor, measurement);
    const auto defaults = sensors::calibration::defaults(source, sensor, measurement);
    json += "{\"gain\":";
    appendJsonFloat(json, value.gain);
    json += ",\"offset_input_v\":";
    appendJsonFloat(json, value.offsetInputV);
    json += ",\"default_gain\":";
    appendJsonFloat(json, defaults.gain);
    json += ",\"default_offset_input_v\":";
    appendJsonFloat(json, defaults.offsetInputV);
    json += '}';
}

void appendSensorJson(String& json, const char* id, const char* label, sensors::SensorId sensor,
                      sensor_mode::Mode mode) {
    sensors::Reading reading{};
    const bool hasReading = sensors::getLatest(sensor, reading);
    sensors::mapping::PhysicalSensorId physical{};
    const bool mapped =
        sensors::mapping::physicalForLogical(mode, sensor, physical);
    const bool observed = hasReading &&
        (reading.state == sensors::ReadingState::Valid || reading.state == sensors::ReadingState::OutOfRange);
    json += "{\"id\":\"";
    json += id;
    json += "\",\"label\":\"";
    json += label;
    json += "\",\"physical_sensor\":";
    if (mapped) {
        json += '"';
        json += sensors::mapping::physicalId(physical);
        json += '"';
    } else {
        json += "null";
    }
    json += ",\"configured\":";
    json += (hasReading && reading.configured) ? "true" : "false";
    json += ",\"observed\":";
    json += (observed ? "true" : "false");
    json += ",\"state\":\"";
    json += (hasReading ? readingStateName(reading.state) : "waiting");
    json += "\",\"sample_age_ms\":";
    if (hasReading) json += String(static_cast<uint32_t>(millis() - reading.timestamp_ms));
    else json += "null";
    json += ",\"voltage\":";
    appendJsonFloat(json, hasReading ? reading.voltage : NAN);
    json += ",\"current\":";
    appendJsonFloat(json, hasReading ? reading.current : NAN);
    json += ",\"power\":";
    appendJsonFloat(json, hasReading ? reading.power : NAN);
    json += ",\"input_voltage_v\":";
    appendJsonFloat(json, hasReading ? reading.voltageInputV : NAN);
    json += ",\"input_current_v\":";
    appendJsonFloat(json, hasReading ? reading.currentInputV : NAN);
    json += ",\"current_multiplier\":";
    json += String(mapped
        ? sensors::mapping::currentMultiplier(mode, physical) : 1);
    json += ",\"duty\":{\"state\":\"";
    json += (hasReading ? dutyStateName(reading.dutyState) : "not_reported");
    json += "\",\"value\":";
    appendJsonFloat(json, hasReading ? reading.dutyCycle : NAN);
    json += "},\"calibration\":";
    sensors::calibration::Source calibrationSource{};
    if (mapped && calibrationSourceForMode(mode, calibrationSource)) {
        json += "{\"editable\":";
        json += (hasReading && reading.configured) ? "true" : "false";
        json += ",\"voltage\":";
        appendCalibrationValue(
            json, calibrationSource, static_cast<uint8_t>(physical),
                               sensors::calibration::Measurement::Voltage);
        json += ",\"current\":";
        appendCalibrationValue(
            json, calibrationSource, static_cast<uint8_t>(physical),
                               sensors::calibration::Measurement::Current);
        json += '}';
    } else {
        json += "null";
    }
    json += '}';
}

void appendPhysicalSensorJson(
    String& json, sensor_mode::Mode mode,
    sensors::mapping::PhysicalSensorId physical,
    const sensors::mapping::Entry& entry) {
    const uint8_t index = static_cast<uint8_t>(physical);
    sensors::Reading reading{};
    const bool hasReading = sensors::getLatestPhysical(index, reading);
    const bool observed = hasReading &&
        (reading.state == sensors::ReadingState::Valid ||
         reading.state == sensors::ReadingState::OutOfRange);
    json += "{\"id\":\"";
    json += sensors::mapping::physicalId(physical);
    json += "\",\"label\":\"";
    json += sensors::mapping::physicalLabel(physical);
    json += "\",\"role\":\"";
    json += sensors::mapping::roleName(entry.role);
    json += "\",\"current_direction\":\"";
    json += sensors::mapping::directionName(entry.currentDirection);
    json += "\",\"current_multiplier\":";
    json += String(sensors::mapping::multiplier(entry.currentDirection));
    json += ",\"source_configured\":";
    json += (hasReading && reading.configured) ? "true" : "false";
    json += ",\"observed\":";
    json += (observed ? "true" : "false");
    json += ",\"state\":\"";
    json += (hasReading ? readingStateName(reading.state) : "waiting");
    json += "\",\"sample_age_ms\":";
    if (hasReading) {
        json += String(static_cast<uint32_t>(millis() - reading.timestamp_ms));
    } else {
        json += "null";
    }
    json += ",\"voltage\":";
    appendJsonFloat(json, hasReading ? reading.voltage : NAN);
    json += ",\"current\":";
    appendJsonFloat(json, hasReading ? reading.current : NAN);
    json += ",\"power\":";
    appendJsonFloat(json, hasReading ? reading.power : NAN);
    json += ",\"input_voltage_v\":";
    appendJsonFloat(json, hasReading ? reading.voltageInputV : NAN);
    json += ",\"input_current_v\":";
    appendJsonFloat(json, hasReading ? reading.currentInputV : NAN);
    json += ",\"calibration\":";
    sensors::calibration::Source calibrationSource{};
    if (calibrationSourceForMode(mode, calibrationSource)) {
        json += "{\"editable\":";
        json += (hasReading && reading.configured) ? "true" : "false";
        json += ",\"voltage\":";
        appendCalibrationValue(json, calibrationSource, index,
                               sensors::calibration::Measurement::Voltage);
        json += ",\"current\":";
        appendCalibrationValue(json, calibrationSource, index,
                               sensors::calibration::Measurement::Current);
        json += '}';
    } else {
        json += "null";
    }
    json += '}';
}

void appendSensorMappingJson(String& json, sensor_mode::Mode mode) {
    const sensors::mapping::Profile profile = sensors::mapping::get(mode);
    json = "{\"api_version\":1,\"source\":\"";
    json += sensor_mode::name(mode);
    json += "\",\"requires_restart\":true,\"physical_sensors\":[";
    for (uint8_t index = 0; index < sensors::mapping::kPhysicalSensorCount;
         ++index) {
        if (index) json += ',';
        appendPhysicalSensorJson(
            json, mode,
            static_cast<sensors::mapping::PhysicalSensorId>(index),
            profile.physical[index]);
    }
    json += "]}";
}

void webSensorMapping() {
    const sensor_mode::Mode mode = sensor_mode::get();
    if (server->method() == HTTP_GET) {
        String json;
        json.reserve(2600);
        appendSensorMappingJson(json, mode);
        server->sendHeader("Cache-Control", "no-store");
        server->send(200, "application/json", json);
        return;
    }

    const String body = server->arg("plain");
    String requestedSource;
    if (!http_utils::jsonString(body, "source", requestedSource) ||
        requestedSource != sensor_mode::name(mode)) {
        server->send(
            409, "application/json",
            "{\"error\":\"mapping source must match the active sensor source\"}");
        return;
    }

    sensors::mapping::Profile candidate{};
    for (uint8_t index = 0; index < sensors::mapping::kPhysicalSensorCount;
         ++index) {
        char roleKey[24];
        char directionKey[36];
        snprintf(roleKey, sizeof(roleKey), "sensor%u_role", index + 1);
        snprintf(directionKey, sizeof(directionKey),
                 "sensor%u_current_direction", index + 1);
        String role;
        String direction;
        if (!http_utils::jsonString(body, roleKey, role) ||
            !http_utils::jsonString(body, directionKey, direction) ||
            !sensors::mapping::parseRole(
                role.c_str(), candidate.physical[index].role) ||
            !sensors::mapping::parseDirection(
                direction.c_str(),
                candidate.physical[index].currentDirection)) {
            server->send(
                400, "application/json",
                "{\"error\":\"invalid or incomplete sensor mapping\"}");
            return;
        }
    }
    if (!sensors::mapping::isValid(candidate)) {
        server->send(
            400, "application/json",
            "{\"error\":\"map Solar and Load exactly once; Battery at most once\"}");
        return;
    }
    if (!sensors::mapping::set(mode, candidate)) {
        server->send(
            500, "application/json",
            "{\"error\":\"could not persist sensor mapping\"}");
        return;
    }

    server->sendHeader("Cache-Control", "no-store");
    server->send(
        200, "application/json", "{\"ok\":true,\"restarting\":true}");
    delay(250);
    ESP.restart();
}

void sendHistoryJobState(uint32_t job, const char* resource) {
    const history_query_service::JobState state = history_query_service::jobState(job);
    server->sendHeader("Cache-Control", "no-store");
    if (state == history_query_service::JobState::Queued ||
        state == history_query_service::JobState::Running ||
        state == history_query_service::JobState::Ready) {
        server->sendHeader("Retry-After", "1");
        server->send(202, "application/json", String("{\"state\":\"") +
            history_query_service::jobStateName(state) + "\"}");
        return;
    }
    if (state == history_query_service::JobState::Gone) {
        server->send(410, "application/json", String("{\"error\":\"") + resource +
            " job expired or was already consumed\"}");
        return;
    }
    server->send(404, "application/json", String("{\"error\":\"") + resource +
        " job not found\"}");
}

// V1 sensor diagnostics are intentionally a read model, not an operational
// calculation feed. Finite observed values remain visible when out of range;
// unavailable values are JSON null. The Power and History APIs continue to
// expose only calculation-eligible values.
void webSensors() {
    const sensor_mode::Mode mode = sensor_mode::get();
    String json;
    json.reserve(3600);
    json = "{\"api_version\":1,\"source\":{\"mode\":\"";
    switch (mode) {
        case sensor_mode::Mode::Adc: json += "adc"; break;
        case sensor_mode::Mode::Ads1115: json += "ads1115"; break;
        case sensor_mode::Mode::Uart: json += "uart"; break;
        case sensor_mode::Mode::Demo: json += "demo"; break;
    }
    json += "\",\"label\":\"";
    json += sensor_mode::label();
    json += "\",\"transport\":";
    if (mode == sensor_mode::Mode::Uart) {
        const sensors::pm1::Diagnostics diagnostics = sensors::getUartDiagnostics();
        const uint32_t age = sensors::getUartLastValidAgeMs();
        const bool receiving = diagnostics.hasValidFrame && age < sensors::pm1::kStaleAfterMs;
        json += "{\"type\":\"uart\",\"state\":\"";
        json += (receiving ? "receiving" : (diagnostics.hasValidFrame ? "stale" : "waiting"));
        json += "\",\"connected\":";
        json += (receiving ? "true" : "false");
        json += ",\"last_valid_age_ms\":";
        if (diagnostics.hasValidFrame) json += String(age);
        else json += "null";
        json += ",\"channel_mask\":";
        if (diagnostics.hasValidFrame) json += String(diagnostics.mask);
        else json += "null";
        json += ",\"sequence\":";
        if (diagnostics.hasValidFrame) json += String(diagnostics.sequence);
        else json += "null";
        json += ",\"source_uptime_ms\":";
        if (diagnostics.hasValidFrame) json += String(diagnostics.sourceUptimeMs);
        else json += "null";
        json += ",\"valid_frames\":";
        json += String(diagnostics.validFrames);
        json += ",\"invalid_frames\":";
        json += String(diagnostics.invalidFrames);
        json += ",\"checksum_errors\":";
        json += String(diagnostics.checksumErrors);
        json += ",\"overflow_frames\":";
        json += String(diagnostics.overflowFrames);
        json += ",\"duplicate_frames\":";
        json += String(diagnostics.duplicateFrames);
        json += ",\"sequence_gap_events\":";
        json += String(diagnostics.sequenceGapEvents);
        json += ",\"missing_frames\":";
        json += String(diagnostics.missingFrames);
        json += ",\"sequence_resets\":";
        json += String(diagnostics.sequenceResets);
        json += ",\"sequence_wraps\":";
        json += String(diagnostics.sequenceWraps);
        json += ",\"last_error\":\"";
        json += sensors::pm1::parseErrorLabel(diagnostics.lastError);
        json += "\"}";
    } else if (mode == sensor_mode::Mode::Ads1115) {
        const sensors::Ads1115Diagnostics diagnostics = sensors::getAds1115Diagnostics();
        constexpr uint32_t kHealthyConversionAgeMs = 2000;
        const bool hasSuccessfulConversion = diagnostics.successfulConversions > 0;
        const uint32_t successAgeMs = hasSuccessfulConversion
            ? millis() - diagnostics.lastSuccessMs : UINT32_MAX;
        const bool healthy = diagnostics.ready && hasSuccessfulConversion &&
                             successAgeMs <= kHealthyConversionAgeMs &&
                             diagnostics.consecutiveFailures < 4;
        const char* state = healthy ? "receiving" :
            (diagnostics.ready ? (hasSuccessfulConversion ? "degraded" : "initializing") :
                                 "unavailable");
        json += "{\"type\":\"i2c\",\"state\":\"";
        json += state;
        json += "\",\"connected\":";
        json += healthy ? "true" : "false";
        json += ",\"initialized\":";
        json += diagnostics.ready ? "true" : "false";
        json += ",\"address\":";
        json += String(diagnostics.address);
        json += ",\"successful_conversions\":";
        json += String(diagnostics.successfulConversions);
        json += ",\"failed_conversions\":";
        json += String(diagnostics.failedConversions);
        json += ",\"lock_timeouts\":";
        json += String(diagnostics.lockTimeouts);
        json += ",\"last_conversion_us\":";
        json += String(diagnostics.lastConversionUs);
        json += ",\"last_success_age_ms\":";
        if (hasSuccessfulConversion) json += String(successAgeMs);
        else json += "null";
        json += ",\"consecutive_failures\":";
        json += String(diagnostics.consecutiveFailures);
        json += ",\"bus_errors\":";
        json += String(diagnostics.busErrors);
        json += '}';
    } else {
        json += "null";
    }
    json += "},\"channels\":[";
    appendSensorJson(json, "in", "Solar", sensors::SENSOR_SOLAR, mode);
    json += ',';
    appendSensorJson(json, "out", "Load", sensors::SENSOR_LOAD, mode);
    json += ',';
    appendSensorJson(json, "aux", "Battery", sensors::SENSOR_BATTERY, mode);
    json += "]}";
    server->sendHeader("Cache-Control", "no-store");
    server->send(200, "application/json", json);
}

void webSensorCalibration() {
    const sensor_mode::Mode mode = sensor_mode::get();
    sensors::calibration::Source source{};
    if (!calibrationSourceForMode(mode, source)) {
        server->send(409, "application/json", "{\"error\":\"active sensor source does not use calibration\"}");
        return;
    }
    const String body = server->arg("plain");
    String sensorName, measurementName;
    float gain = NAN, offset = NAN;
    if (!http_utils::jsonString(body, "sensor", sensorName) ||
        !http_utils::jsonString(body, "measurement", measurementName) ||
        !http_utils::jsonFloat(body, "gain", gain) ||
        !http_utils::jsonFloat(body, "offset_input_v", offset)) {
        server->send(400, "application/json", "{\"error\":\"invalid calibration request\"}");
        return;
    }
    uint8_t physicalSensor = sensors::mapping::kPhysicalSensorCount;
    sensors::Reading reading{};
    bool hasReading = false;
    sensors::SensorId logical = sensors::SENSOR_COUNT;
    if (sensorName == "in") logical = sensors::SENSOR_SOLAR;
    else if (sensorName == "out") logical = sensors::SENSOR_LOAD;
    else if (sensorName == "aux") logical = sensors::SENSOR_BATTERY;
    if (logical < sensors::SENSOR_COUNT) {
        sensors::mapping::PhysicalSensorId physical;
        if (sensors::mapping::physicalForLogical(mode, logical, physical)) {
            physicalSensor = static_cast<uint8_t>(physical);
            hasReading = sensors::getLatest(logical, reading);
        }
    } else {
        for (uint8_t index = 0;
             index < sensors::mapping::kPhysicalSensorCount; ++index) {
            const auto physical =
                static_cast<sensors::mapping::PhysicalSensorId>(index);
            if (sensorName == sensors::mapping::physicalId(physical)) {
                physicalSensor = index;
                hasReading = sensors::getLatestPhysical(index, reading);
                break;
            }
        }
    }
    sensors::calibration::Measurement measurement;
    if (measurementName == "voltage") measurement = sensors::calibration::Measurement::Voltage;
    else if (measurementName == "current") measurement = sensors::calibration::Measurement::Current;
    else {
        server->send(400, "application/json", "{\"error\":\"invalid measurement\"}");
        return;
    }
    const sensors::calibration::Value value{gain, offset};
    if (physicalSensor >= sensors::mapping::kPhysicalSensorCount ||
        !sensors::calibration::isValid(measurement, value)) {
        server->send(400, "application/json", "{\"error\":\"calibration value is outside allowed limits\"}");
        return;
    }
    if (!hasReading || !reading.configured) {
        server->send(409, "application/json", "{\"error\":\"sensor channel is not configured\"}");
        return;
    }
    if (!sensors::calibration::set(
            source, physicalSensor, measurement, value)) {
        server->send(500, "application/json", "{\"error\":\"could not persist calibration\"}");
        return;
    }
    webSensors();
}

// Browser history is an explicitly requested, bounded background job. The
// query service owns LittleFS work. This handler leases its completed result
// and serializes one bounded record at a time, avoiding two full-size copies.
void webHistoryQuery() {
    if (!server->hasArg("job")) {
        auto kind = history_query_service::UsageQueryKind::Calendar;
        historical_storage::CalendarRange range = historical_storage::CalendarRange::Today;
        uint32_t lookbackMinutes = 0;
        uint16_t defaultBucketMinutes = 30;
        const String rangeArg = server->arg("range");
        if (rangeArg == "last1hour") {
            kind = history_query_service::UsageQueryKind::Rolling;
            lookbackMinutes = 60; defaultBucketMinutes = 1;
        } else if (rangeArg == "last6hours") {
            kind = history_query_service::UsageQueryKind::Rolling;
            lookbackMinutes = 360; defaultBucketMinutes = 10;
        } else if (rangeArg == "last24hours") {
            kind = history_query_service::UsageQueryKind::Rolling;
            lookbackMinutes = 1440; defaultBucketMinutes = 30;
        } else if (rangeArg == "last2days") {
            kind = history_query_service::UsageQueryKind::Rolling;
            lookbackMinutes = 2880; defaultBucketMinutes = 60;
        } else if (rangeArg == "lastweek") {
            kind = history_query_service::UsageQueryKind::Rolling;
            lookbackMinutes = 10080; defaultBucketMinutes = 240;
        } else if (rangeArg == "sinceboot") {
            kind = history_query_service::UsageQueryKind::SinceBoot;
            defaultBucketMinutes = 0;
        }
        else if (rangeArg == "yesterday") range = historical_storage::CalendarRange::Yesterday;
        else if (rangeArg == "lasttwoweeks") range = historical_storage::CalendarRange::LastTwoWeeks;
        else if (rangeArg == "all") { range = historical_storage::CalendarRange::All; defaultBucketMinutes = 0; }
        else if (!rangeArg.isEmpty() && rangeArg != "today") {
            server->send(400, "application/json", "{\"error\":\"invalid history range\"}");
            return;
        }
        uint16_t bucketMinutes = defaultBucketMinutes;
        if (server->hasArg("bucket_minutes")) {
            const int requested = server->arg("bucket_minutes").toInt();
            bucketMinutes = ((kind == history_query_service::UsageQueryKind::Calendar &&
                              range == historical_storage::CalendarRange::All) ||
                             kind == history_query_service::UsageQueryKind::SinceBoot) &&
                                requested == 0
                ? 0 : constrain(requested, 1, 1440);
        }
        const uint32_t job = history_query_service::requestUsage({kind,
            range, lookbackMinutes, bucketMinutes});
        if (!job) {
            server->sendHeader("Retry-After", "1");
            server->send(503, "application/json", "{\"error\":\"history queue full or unavailable\"}");
            return;
        }
        server->sendHeader("Cache-Control", "no-store");
        server->send(202, "application/json", String("{\"job\":") + job + "}");
        return;
    }

    const uint32_t job = static_cast<uint32_t>(server->arg("job").toInt());
    history_query_service::UsageResultView result{};
    if (!history_query_service::acquireUsage(job, result)) {
        sendHistoryJobState(job, "history");
        return;
    }

    uint8_t header[history_response_encoder::kHeaderBytes];
    history_response_encoder::encodeHeader(header, job, result.count, result.status);
    const size_t responseBytes = sizeof(header) + result.count * history_response_encoder::kRecordBytes;
    server->sendHeader("Cache-Control", "no-store");
    server->setContentLength(responseBytes);
    server->send(200, "application/vnd.viewe.history-v1", "");
    WiFiClient client = server->client();
    client.setConnectionTimeout(1500);
    bool sent = http_utils::writeClient(client, header, sizeof(header));
    uint8_t record[history_response_encoder::kRecordBytes];
    for (size_t i = 0; sent && i < result.count; ++i) {
        history_response_encoder::encodeRecord(record, result.buckets[i]);
        sent = http_utils::writeClient(client, record, sizeof(record));
    }
    history_query_service::releaseUsage(job);
}

bool clearPreferences(const char* name) {
    Preferences prefs;
    if (!prefs.begin(name, false)) return false;
    const bool cleared = prefs.clear();
    prefs.end();
    return cleared;
}

void webSetup() {
    if (server->method() == HTTP_GET) {
        String response = String("{\"api_version\":1,\"hostname\":\"") +
            device_identity::getDeviceId() + "\",\"sensor_mode\":\"" +
            sensorModeName(sensor_mode::get()) + "\",\"appearance\":\"" +
            display_web_api::appearanceModeName() + "\",\"status_display_mode\":\"" +
            status_display::modeName() + "\"}";
        server->sendHeader("Cache-Control", "no-store");
        server->send(200, "application/json", response);
        return;
    }

    const String body = server->arg("plain");
    String hostname, requestedSensorMode, requestedAppearance, requestedStatusDisplayMode;
    bool resetSetup = false, resetWifi = false, resetCalibration = false, resetUsage = false;
    if (!http_utils::jsonString(body, "hostname", hostname) ||
        !http_utils::jsonString(body, "sensor_mode", requestedSensorMode) ||
        !http_utils::jsonString(body, "appearance", requestedAppearance) ||
        !http_utils::jsonBool(body, "reset_setup", resetSetup) ||
        !http_utils::jsonBool(body, "reset_wifi", resetWifi) ||
        !http_utils::jsonBool(body, "reset_calibration", resetCalibration) ||
        !http_utils::jsonBool(body, "reset_usage", resetUsage)) {
        server->send(400, "application/json", "{\"error\":\"invalid setup request\"}");
        return;
    }
    requestedStatusDisplayMode = status_display::modeName();
    if (body.indexOf("\"status_display_mode\"") >= 0 &&
        !http_utils::jsonString(body, "status_display_mode", requestedStatusDisplayMode)) {
        server->send(400, "application/json", "{\"error\":\"invalid status display mode\"}");
        return;
    }
    if (!device_identity::isValidDeviceId(hostname.c_str())) {
        server->send(400, "application/json", "{\"error\":\"Use lowercase letters, numbers, and hyphens (max 31); it cannot start or end with a hyphen.\"}");
        return;
    }

    sensor_mode::Mode mode;
    if (requestedSensorMode == "adc") mode = sensor_mode::Mode::Adc;
    else if (requestedSensorMode == "ads1115") mode = sensor_mode::Mode::Ads1115;
    else if (requestedSensorMode == "uart") mode = sensor_mode::Mode::Uart;
    else if (requestedSensorMode == "demo") mode = sensor_mode::Mode::Demo;
    else {
        server->send(400, "application/json", "{\"error\":\"invalid sensor mode\"}");
        return;
    }
    if (!sensor_mode::isSupported(mode)) {
        server->send(400, "application/json", "{\"error\":\"sensor mode is not supported by this hardware profile\"}");
        return;
    }
    if (!display_web_api::isValidAppearance(requestedAppearance)) {
        server->send(400, "application/json", "{\"error\":\"invalid device appearance\"}");
        return;
    }
    status_display::Mode statusDisplayMode;
    if (requestedStatusDisplayMode == "summary") statusDisplayMode = status_display::Mode::Summary;
    else if (requestedStatusDisplayMode == "dense") statusDisplayMode = status_display::Mode::Dense;
    else {
        server->send(400, "application/json", "{\"error\":\"invalid status display mode\"}");
        return;
    }

    if (resetSetup) {
        if (!clearPreferences("device") || !clearPreferences("sensors") ||
            !clearPreferences("sensor_map") ||
            !clearPreferences("appearance") || !clearPreferences("status_oled")) {
            server->send(500, "application/json", "{\"error\":\"Could not reset setup preferences.\"}");
            return;
        }
    } else {
        if (hostname != device_identity::getDeviceId() &&
            !device_identity::setDeviceId(hostname.c_str())) {
            server->send(500, "application/json", "{\"error\":\"Could not save hostname.\"}");
            return;
        }
        if (mode != sensor_mode::get() && !sensor_mode::set(mode)) {
            server->send(500, "application/json", "{\"error\":\"Could not save sensor mode.\"}");
            return;
        }
        display_web_api::setAppearance(requestedAppearance);
        if (hardware_profile::kHasStatusDisplay && statusDisplayMode != status_display::mode() &&
            !status_display::setMode(statusDisplayMode)) {
            server->send(500, "application/json", "{\"error\":\"Could not save status display mode.\"}");
            return;
        }
    }
    if (resetCalibration && !clearPreferences("sensor_cal")) {
        server->send(500, "application/json", "{\"error\":\"Could not reset sensor calibration.\"}");
        return;
    }
    if (resetWifi && !network_manager::clearSavedCredentials()) {
        server->send(500, "application/json", "{\"error\":\"Could not reset Wi-Fi credentials.\"}");
        return;
    }
    if (resetUsage && !historical_storage::clearAll()) {
        server->send(500, "application/json", "{\"error\":\"Could not reset usage history.\"}");
        return;
    }

    server->sendHeader("Cache-Control", "no-store");
    server->send(200, "application/json", "{\"ok\":true,\"restarting\":true}");
    delay(250); // let the browser receive confirmation before settings take effect
    ESP.restart();
}

void webDebug() {
    constexpr uint32_t kInternalCaps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    constexpr uint32_t kPsramCaps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    const size_t internalTotal = heap_caps_get_total_size(kInternalCaps);
    const size_t internalFree = heap_caps_get_free_size(kInternalCaps);
    const size_t psramTotal = heap_caps_get_total_size(kPsramCaps);
    const size_t psramFree = heap_caps_get_free_size(kPsramCaps);
    const size_t storageTotal = LittleFS.totalBytes();
    const uint32_t validationRemaining = ota_service::validationRemainingMs();
    history_query_service::Timing timing{};
    history_query_service::getTiming(timing);
    time_service::Anchor timeAnchor{};
    const char* timeSource = time_service::getCurrentAnchor(timeAnchor)
        ? time_service::sourceName(timeAnchor.source) : "unanchored";

    String response;
    response.reserve(1400);
    response = String("{\"api_version\":1,\"lvgl\":\"") + display_web_api::lvglVersion() +
        "\",\"sdk\":\"" + ESP.getSdkVersion() +
        "\",\"chip\":\"" + ESP.getChipModel() + " rev " + ESP.getChipRevision() +
        "\",\"cpu_mhz\":" + ESP.getCpuFreqMHz() + ",\"flash_mb\":" +
        (ESP.getFlashChipSize() / (1024 * 1024)) + ",\"last_reset\":\"" +
        resetReasonName(esp_reset_reason()) + "\",\"internal_heap\":{\"used_percent\":" +
        (internalTotal ? ((internalTotal - internalFree) * 100 + internalTotal / 2) / internalTotal : 0) +
        ",\"largest_free_kb\":" + (heap_caps_get_largest_free_block(kInternalCaps) / 1024) +
        "},\"psram_heap\":{\"used_percent\":" +
        (psramTotal ? ((psramTotal - psramFree) * 100 + psramTotal / 2) / psramTotal : 0) +
        ",\"largest_free_kb\":" + (heap_caps_get_largest_free_block(kPsramCaps) / 1024) +
        "},\"lvgl_stack\":";
#if POWER_METER_HAS_TOUCH_UI
    response += String("{\"size_bytes\":") + lvgl_port_stack_size_bytes() +
        ",\"minimum_free_bytes\":" + lvgl_port_stack_minimum_free_bytes() + "}";
#else
    response += "null";
#endif
    response += String(",\"storage\":{\"mounted\":") + (storageTotal ? "true" : "false") +
        ",\"used_kb\":" + (LittleFS.usedBytes() / 1024) + ",\"total_kb\":" +
        (storageTotal / 1024) + "},\"ota\":{\"health\":\"" + ota_service::healthStatus() +
        "\",\"validation_remaining_ms\":" + validationRemaining + ",\"running_slot\":\"" +
        ota_service::runningPartitionLabel() + "\",\"boot_slot\":\"" + ota_service::bootPartitionLabel() +
        "\",\"image_state\":\"" + ota_service::runningImageState() + "\",\"rollback_detected\":" +
        (ota_service::rollbackDetected() ? "true" : "false") + "},\"history_query\":{\"last_duration_ms\":" +
        timing.lastDurationMs + ",\"max_duration_ms\":" + timing.maxDurationMs +
        ",\"records_read\":" + timing.lastRecordsRead + ",\"files_read\":" + timing.lastFilesRead +
        ",\"was_usage\":" + (timing.lastWasUsage ? "true" : "false") +
        "},\"time_source\":\"" + timeSource + "\",\"web_build\":\"" +
        web_assets::kBuildId + "\",\"data_storage_percent\":" +
        (storageTotal ? (LittleFS.usedBytes() * 100U) / storageTotal : 0) +
        ",\"ws_connections\":" + live_websocket_service::clientCount() +
        ",\"ws_connection_limit\":" + live_websocket_service::clientLimit() + "}";
    server->sendHeader("Cache-Control", "no-store");
    server->send(200, "application/json", response);
}

void webUpdateStatus() {
    internet_update_service::Status status{};
    if (!internet_update_service::getStatus(status)) {
        server->send(503, "application/json",
                     "{\"error\":\"Update status is unavailable.\"}");
        return;
    }
    String response;
    response.reserve(640);
    response = String("{\"api_version\":1,\"state\":\"") +
        internet_update_service::stateName(status.state) +
        "\",\"automatic\":" + (status.automatic ? "true" : "false") +
        ",\"busy\":" + (status.busy ? "true" : "false") +
        ",\"progress_percent\":" + status.progressPercent +
        ",\"update_date_unix_ms\":" +
        static_cast<long long>(status.updateDateUnixSeconds * 1000LL) +
        ",\"last_check_unix_ms\":" +
        static_cast<long long>(status.lastCheckUnixSeconds * 1000LL) +
        ",\"next_check_unix_ms\":" +
        static_cast<long long>(status.nextCheckUnixSeconds * 1000LL) +
        ",\"current_version\":";
    http_utils::appendJsonString(response, status.currentVersion);
    response += ",\"available_version\":";
    http_utils::appendJsonString(response, status.availableVersion);
    response += ",\"blocked_version\":";
    http_utils::appendJsonString(response, status.blockedVersion);
    response += ",\"error\":";
    http_utils::appendJsonString(response, status.error);
    response += "}";
    server->sendHeader("Cache-Control", "no-store");
    server->send(200, "application/json", response);
}

void webUpdateCheck() {
    if (!internet_update_service::requestCheck()) {
        server->send(409, "application/json",
                     "{\"ok\":false,\"error\":\"An update operation is already active or the new image is still being validated.\"}");
        return;
    }
    server->send(202, "application/json", "{\"ok\":true,\"checking\":true}");
}

void webUpdateInstall() {
    if (!internet_update_service::requestInstall()) {
        server->send(409, "application/json",
                     "{\"ok\":false,\"error\":\"No installable update is available or an update operation is active.\"}");
        return;
    }
    server->send(202, "application/json", "{\"ok\":true,\"installing\":true}");
}

void webUpdateSettings() {
    bool automatic = false;
    if (!http_utils::jsonBool(server->arg("plain"), "automatic", automatic)) {
        server->send(400, "application/json",
                     "{\"ok\":false,\"error\":\"automatic must be a boolean.\"}");
        return;
    }
    if (!internet_update_service::setAutomatic(automatic)) {
        server->send(500, "application/json",
                     "{\"ok\":false,\"error\":\"Could not save update settings.\"}");
        return;
    }
    server->send(200, "application/json", "{\"ok\":true}");
}

void browserTimeAnchor() {
    const String body = server->arg("plain");
    int64_t unixMs = 0;
    // Keep obviously bad browser clocks out of persisted history. This is a
    // sanity bound rather than a promise about the product's supported life.
    constexpr int64_t kEarliestPlausibleUnixMs = 1577836800000LL; // 2020-01-01 UTC
    constexpr int64_t kLatestPlausibleUnixMs = 4102444800000LL;   // 2100-01-01 UTC
    if (!http_utils::jsonInteger64(body, "unix_ms", unixMs) ||
        unixMs < kEarliestPlausibleUnixMs || unixMs >= kLatestPlausibleUnixMs) {
        server->send(400, "application/json",
                    "{\"ok\":false,\"error\":\"unix_ms must be an integer browser epoch in milliseconds between 2020 and 2100\"}");
        return;
    }

    int64_t parsedOffset = time_service::utcOffsetMinutes();
    const bool offsetProvided = body.indexOf("\"utc_offset_minutes\"") >= 0;
    if (offsetProvided &&
        (!http_utils::jsonInteger64(body, "utc_offset_minutes", parsedOffset) || parsedOffset < -840 || parsedOffset > 840)) {
        server->send(400, "application/json",
                    "{\"ok\":false,\"error\":\"utc_offset_minutes must be an integer from -840 to 840\"}");
        return;
    }

    const int16_t offsetMinutes = static_cast<int16_t>(parsedOffset);
    // A browser Date.now() sample received over the LAN is useful but not a
    // precision clock measurement. Preserve that distinction in the anchor.
    constexpr uint32_t kBrowserUncertaintyMs = 1000;
    if (!time_service::submitAnchor(unixMs, time_service::AnchorSource::Browser,
                                    offsetMinutes, kBrowserUncertaintyMs)) {
        server->send(503, "application/json",
                    "{\"ok\":false,\"error\":\"time anchor could not be persisted\"}");
        return;
    }
    device_state::changed(device_state::Domain::Time);

    server->send(200, "application/json",
                String("{\"ok\":true,\"source\":\"browser\",\"unix_ms\":") + unixMs +
                ",\"utc_offset_minutes\":" + offsetMinutes +
                ",\"offset_updated\":" + (offsetProvided ? "true" : "false") + "}");
}

void webCycles() {
    if (server->method() == HTTP_POST) {
        uint32_t hour = 0;
        if (!http_utils::jsonUnsigned(server->arg("plain"), "end_hour", hour) || hour >= 24) {
            server->send(400, "application/json",
                        "{\"ok\":false,\"error\":\"end_hour must be an integer from 0 to 23\"}");
            return;
        }
        if (!energy_cycle::setEndHour(static_cast<uint8_t>(hour))) {
            server->send(503, "application/json",
                        "{\"ok\":false,\"error\":\"cycle end time could not be persisted\"}");
            return;
        }
        server->sendHeader("Cache-Control", "no-store");
        server->send(200, "application/json",
                    String("{\"ok\":true,\"end_hour\":") + hour + "}");
        return;
    }

    if (!server->hasArg("job")) {
        const uint32_t job = history_query_service::requestCycles();
        if (!job) {
            server->sendHeader("Retry-After", "1");
            server->send(503, "application/json", "{\"error\":\"history queue full or unavailable\"}");
            return;
        }
        server->sendHeader("Cache-Control", "no-store");
        server->send(202, "application/json", String("{\"job\":") + job + "}");
        return;
    }

    const uint32_t job = static_cast<uint32_t>(server->arg("job").toInt());
    energy_cycle::Summary summaries[energy_cycle::kRecentCycleCount]{};
    size_t count = 0;
    if (!history_query_service::takeCycles(job, summaries, energy_cycle::kRecentCycleCount, count)) {
        sendHistoryJobState(job, "cycle");
        return;
    }

    String response;
    response.reserve(1800);
    response = String("{\"api_version\":1,\"end_hour\":") + energy_cycle::endHour() +
        ",\"efficiency_percent\":80,\"utc_offset_minutes\":" +
        time_service::utcOffsetMinutes() + ",\"cycles\":[";
    for (size_t i = 0; i < count; ++i) {
        const auto& summary = summaries[i];
        if (i) response += ',';
        response += String("{\"start_unix_ms\":") + summary.startUnixMs +
            ",\"end_unix_ms\":" + summary.endUnixMs +
            ",\"expected_ms\":" + summary.expectedMs +
            ",\"valid_coverage_ms\":" + summary.validCoverageMs +
            ",\"charged_wh\":";
        response += summary.chargeAvailable ? String(summary.chargeWh, 2) : "null";
        response += ",\"used_wh\":";
        response += summary.useAvailable ? String(summary.useWh, 2) : "null";
        response += ",\"net_wh\":";
        response += summary.netAvailable ? String(summary.netWh, 2) : "null";
        response += String(",\"current\":") + (summary.current ? "true" : "false") +
            ",\"incomplete\":" + (summary.incomplete ? "true" : "false") +
            ",\"quality_flags\":" + summary.qualityFlags + "}";
    }
    response += "]}";
    server->sendHeader("Cache-Control", "no-store");
    server->send(200, "application/json", response);
}

const char* networkStateName(network_manager::NetworkState state) {
    switch (state) {
        case network_manager::NetworkState::Disconnected: return "disconnected";
        case network_manager::NetworkState::ConnectingSta: return "connecting";
        case network_manager::NetworkState::ConnectedStaLocal: return "connected_local";
        case network_manager::NetworkState::ConnectedStaInternet: return "connected_internet";
    }
    return "disconnected";
}

const char* connectionPhaseName(network_manager::ConnectionPhase phase) {
    switch (phase) {
        case network_manager::ConnectionPhase::Idle: return "idle";
        case network_manager::ConnectionPhase::LookingForNetwork: return "looking_for_network";
        case network_manager::ConnectionPhase::ObtainingIp: return "obtaining_ip";
        case network_manager::ConnectionPhase::RetryWaiting: return "retry_waiting";
        case network_manager::ConnectionPhase::ActionRequired: return "action_required";
    }
    return "idle";
}

const char* connectionFailureName(network_manager::ConnectionFailure failure) {
    switch (failure) {
        case network_manager::ConnectionFailure::None: return "none";
        case network_manager::ConnectionFailure::NetworkNotFound: return "network_not_found";
        case network_manager::ConnectionFailure::AuthenticationFailed: return "authentication_failed";
        case network_manager::ConnectionFailure::IncompatibleSecurity: return "incompatible_security";
        case network_manager::ConnectionFailure::ConnectionFailed: return "connection_failed";
        case network_manager::ConnectionFailure::TimedOut: return "timed_out";
        case network_manager::ConnectionFailure::LinkLost: return "link_lost";
    }
    return "connection_failed";
}

const char* recoveryStateName(network_manager::RecoveryState state) {
    switch (state) {
        case network_manager::RecoveryState::Disabled: return "disabled";
        case network_manager::RecoveryState::Idle: return "idle";
        case network_manager::RecoveryState::FastRetry: return "fast_retry";
        case network_manager::RecoveryState::Discovering: return "discovering";
        case network_manager::RecoveryState::TryingCandidate: return "trying_candidate";
        case network_manager::RecoveryState::Waiting: return "waiting";
        case network_manager::RecoveryState::Blocked: return "blocked";
    }
    return "disabled";
}

const char* scanStateName(network_manager::ScanState state) {
    switch (state) {
        case network_manager::ScanState::Idle: return "idle";
        case network_manager::ScanState::Starting: return "starting";
        case network_manager::ScanState::Running: return "running";
        case network_manager::ScanState::Succeeded: return "succeeded";
        case network_manager::ScanState::Failed: return "failed";
    }
    return "idle";
}

bool validWifiText(const String& value, size_t minimum, size_t maximum) {
    if (value.length() < minimum || value.length() > maximum) return false;
    for (size_t i = 0; i < value.length(); ++i) {
        if (static_cast<unsigned char>(value[i]) < 0x20) return false;
    }
    return true;
}

void webWifi() {
    const auto stationState = network_manager::getState();
    const bool stationConnected =
        stationState == network_manager::NetworkState::ConnectedStaLocal ||
        stationState == network_manager::NetworkState::ConnectedStaInternet;
    char apSsid[33] = {};
    bool apSecure = true;
    bool apPasswordConfigured = false;
    network_manager::getSavedApSettings(apSsid, sizeof(apSsid), apSecure,
                                        apPasswordConfigured);

    String response;
    response.reserve(4096);
    response = String("{\"api_version\":1,\"state_revision\":") +
        device_state::revision() + ",\"station\":{\"state\":\"" +
        networkStateName(stationState) + "\",\"phase\":\"" +
        connectionPhaseName(network_manager::getConnectionPhase()) +
        "\",\"failure\":\"" +
        connectionFailureName(network_manager::getConnectionFailure()) +
        "\",\"recovery\":\"" +
        recoveryStateName(network_manager::getRecoveryState()) +
        "\",\"ssid\":";
    http_utils::appendJsonString(response, network_manager::getCurrentSsid());
    response += String(",\"ip\":\"") + network_manager::getStaIpAddress() +
        "\",\"rssi\":";
    response += stationConnected ? String(network_manager::getRssi()) : "null";
    response += String(",\"reconnect_seconds\":") +
        network_manager::getReconnectSecondsRemaining() + "},\"scan\":{\"state\":\"" +
        scanStateName(network_manager::getScanState()) +
        "\",\"generation\":" + network_manager::getScanGeneration() +
        ",\"networks\":[";

    const int scanCount = network_manager::getScanResultCount();
    bool firstScan = true;
    for (int i = 0; i < scanCount; ++i) {
        char ssid[33];
        bool secure = false;
        int rssi = 0;
        if (!network_manager::getScanResult(i, ssid, sizeof(ssid), secure, rssi)) continue;
        if (!firstScan) response += ',';
        firstScan = false;
        response += "{\"ssid\":";
        http_utils::appendJsonString(response, ssid);
        response += String(",\"secure\":") + (secure ? "true" : "false") +
            ",\"rssi\":" + rssi + "}";
    }
    response += "]},\"saved_networks\":[";
    const int savedCount = network_manager::getSavedNetworkCount();
    bool firstSaved = true;
    for (int i = 0; i < savedCount; ++i) {
        char ssid[33];
        if (!network_manager::getSavedNetwork(i, ssid, sizeof(ssid))) continue;
        if (!firstSaved) response += ',';
        firstSaved = false;
        http_utils::appendJsonString(response, ssid);
    }
    response += "],\"ap\":{\"enabled\":";
    response += network_manager::isApEnabled() ? "true" : "false";
    response += ",\"ssid\":";
    http_utils::appendJsonString(response, apSsid);
    response += String(",\"secure\":") + (apSecure ? "true" : "false") +
        ",\"password_configured\":" +
        (apPasswordConfigured ? "true" : "false");
    response += String(",\"ip\":\"") +
        (network_manager::isApEnabled() ? network_manager::getApIpAddress() : "") +
        "\",\"client_count\":" + network_manager::getApClientCount() +
        ",\"clients\":[";
    const int clientCount = network_manager::getApClientCount();
    bool firstClient = true;
    for (int i = 0; i < clientCount; ++i) {
        char mac[18];
        if (!network_manager::getApClientMac(i, mac, sizeof(mac))) continue;
        if (!firstClient) response += ',';
        firstClient = false;
        http_utils::appendJsonString(response, mac);
    }
    response += "]}}";
    server->sendHeader("Cache-Control", "no-store");
    server->send(200, "application/json", response);
}

void webWifiStation() {
    const String body = server->arg("plain");
    String action;
    if (!http_utils::jsonString(body, "action", action)) {
        server->send(400, "application/json", "{\"error\":\"invalid station command\"}");
        return;
    }

    if (action == "scan") {
        if (!network_manager::scanNetworks()) {
            server->send(409, "application/json", "{\"error\":\"a network operation is already in progress\"}");
            return;
        }
        server->send(202, "application/json", "{\"ok\":true,\"pending\":true}");
        return;
    }
    if (action == "disconnect") {
        network_manager::disconnect();
        server->send(200, "application/json", "{\"ok\":true}");
        return;
    }

    String ssid;
    if (!http_utils::jsonString(body, "ssid", ssid) ||
        !validWifiText(ssid, 1, 32)) {
        server->send(400, "application/json", "{\"error\":\"SSID must be 1 to 32 bytes without control characters\"}");
        return;
    }
    if (action == "connect") {
        String password;
        if (!http_utils::jsonString(body, "password", password) ||
            !validWifiText(password, 0, 63) ||
            (password.length() > 0 && password.length() < 8)) {
            server->send(400, "application/json", "{\"error\":\"Password must be empty for an open network or 8 to 63 bytes\"}");
            return;
        }
        if (!network_manager::connectTo(ssid.c_str(), password.c_str())) {
            server->send(409, "application/json", "{\"error\":\"a network operation is already in progress\"}");
            return;
        }
        server->send(202, "application/json", "{\"ok\":true,\"pending\":true}");
        return;
    }
    if (action == "connect_saved") {
        if (!network_manager::connectSavedNetwork(ssid.c_str())) {
            server->send(404, "application/json", "{\"error\":\"saved network was not found or is busy\"}");
            return;
        }
        server->send(202, "application/json", "{\"ok\":true,\"pending\":true}");
        return;
    }
    if (action == "forget") {
        if (!network_manager::forgetSavedNetwork(ssid.c_str())) {
            server->send(404, "application/json", "{\"error\":\"saved network was not found\"}");
            return;
        }
        server->send(200, "application/json", "{\"ok\":true}");
        return;
    }
    server->send(400, "application/json", "{\"error\":\"unknown station command\"}");
}

void webWifiAp() {
    const String body = server->arg("plain");
    bool enabled = false;
    if (!http_utils::jsonBool(body, "enabled", enabled)) {
        server->send(400, "application/json", "{\"error\":\"enabled must be boolean\"}");
        return;
    }
    if (!enabled) {
        network_manager::stopAp();
        server->send(200, "application/json", "{\"ok\":true}");
        return;
    }

    String ssid, action, password;
    bool secure = true;
    if (!http_utils::jsonString(body, "ssid", ssid) ||
        !http_utils::jsonString(body, "password_action", action) ||
        !http_utils::jsonBool(body, "secure", secure) ||
        !validWifiText(ssid, 1, 32)) {
        server->send(400, "application/json", "{\"error\":\"invalid access-point settings\"}");
        return;
    }
    network_manager::ApPasswordAction passwordAction;
    if (action == "keep") {
        passwordAction = network_manager::ApPasswordAction::Keep;
    } else if (action == "replace") {
        passwordAction = network_manager::ApPasswordAction::Replace;
    } else if (action == "remove") {
        passwordAction = network_manager::ApPasswordAction::Remove;
    } else {
        server->send(400, "application/json", "{\"error\":\"invalid access-point settings\"}");
        return;
    }

    const bool hasPassword = http_utils::jsonString(body, "password", password);
    const char* replacementPassword = hasPassword ? password.c_str() : nullptr;
    const network_manager::ApStartResult result = network_manager::startAp(
        ssid.c_str(), secure, passwordAction, replacementPassword);
    password = "";
    if (result == network_manager::ApStartResult::InvalidSettings) {
        server->send(400, "application/json", "{\"error\":\"invalid access-point settings\"}");
        return;
    }
    if (result == network_manager::ApStartResult::StartFailed) {
        server->send(500, "application/json", "{\"error\":\"access point could not be started\"}");
        return;
    }
    server->send(200, "application/json", "{\"ok\":true}");
}

void historyFiles() {
    size_t offset = 0;
    size_t limit = 25;
    historical_storage::Dataset dataset = historical_storage::activeDataset();
    if (server->hasArg("offset")) offset = static_cast<size_t>(server->arg("offset").toInt());
    if (server->hasArg("limit")) limit = static_cast<size_t>(server->arg("limit").toInt());
    if (server->hasArg("dataset")) {
        const String requested = server->arg("dataset");
        if (requested == "real") dataset = historical_storage::Dataset::Real;
        else if (requested == "demo") dataset = historical_storage::Dataset::Demo;
        else {
            server->send(400, "application/json", "{\"error\":\"dataset must be real or demo\"}");
            return;
        }
    }
    if (limit == 0 || limit > 50) limit = 25;
    auto* files = static_cast<historical_storage::HistoryFileInfo*>(
        heap_policy::callocPreferred(limit, sizeof(historical_storage::HistoryFileInfo)));
    if (!files) {
        server->send(503, "application/json", "{\"error\":\"history response buffer unavailable\"}");
        return;
    }
    size_t total = 0;
    historical_storage::StorageStats stats{};
    const size_t count = historical_storage::listFilesForDataset(
        dataset, files, limit, offset, &total, &stats);

    String json;
    json.reserve(3072);
    char value[512];
    snprintf(value, sizeof(value),
             "{\"version\":1,\"dataset\":\"%s\",\"flush_interval_minutes\":%u,\"record_size_bytes\":%u,"
             "\"records_per_segment\":%u,\"max_files\":%u,\"file_count\":%u,"
             "\"committed_records\":%lu,\"buffered_records\":%u,"
             "\"committed_bytes\":%lu,\"buffered_bytes\":%lu,"
             "\"offset\":%u,\"limit\":%u,\"total\":%u,\"files\":[",
             dataset == historical_storage::Dataset::Demo ? "demo" : "real",
             historical_storage::kFlushIntervalMinutes,
             static_cast<unsigned>(sizeof(historical_storage::MinuteEnergyRecord)),
             historical_storage::kRecordsPerSegment,
             stats.maxFiles,
             stats.fileCount,
             static_cast<unsigned long>(stats.committedRecords),
             stats.bufferedRecords,
             static_cast<unsigned long>(stats.committedBytes),
             static_cast<unsigned long>(stats.bufferedBytes),
             static_cast<unsigned>(offset), static_cast<unsigned>(limit), static_cast<unsigned>(total));
    json += value;
    for (size_t i = 0; i < count; ++i) {
        const auto& file = files[i];
        const char* state = file.state == historical_storage::FileState::Active ? "active" :
                            file.state == historical_storage::FileState::Closed ? "closed" : "interrupted";
        snprintf(value, sizeof(value),
                 "%s{\"name\":\"%s\",\"session_id\":%lu,\"first_minute\":%lu,"
                 "\"committed_records\":%u,\"buffered_records\":%u,\"bytes\":%lu,"
                 "\"state\":\"%s\",\"fixture\":%s,\"anchored\":%s,\"inferred\":%s,"
                 "\"start_unix_ms\":%lld,\"end_unix_ms\":%lld}",
                 i ? "," : "",
                 file.name, static_cast<unsigned long>(file.sessionId),
                 static_cast<unsigned long>(file.firstMinute), file.committedRecords,
                 file.bufferedRecords, static_cast<unsigned long>(file.bytes), state,
                 file.fixture ? "true" : "false",
                 file.timeFlags & historical_storage::TIME_ANCHORED ? "true" : "false",
                 file.timeFlags & historical_storage::TIME_INFERRED ? "true" : "false",
                 static_cast<long long>(file.startUnixMs), static_cast<long long>(file.endUnixMs));
        json += value;
    }
    json += "]}";
    server->send(200, "application/json", json);
    heap_caps_free(files);
}

} // namespace

void registerRoutes(WebServer& value) {
    server = &value;
    server->on("/api/v1/time/anchor", HTTP_POST, browserTimeAnchor);
    server->on("/api/v1/history/files", HTTP_GET, historyFiles);
    server->on("/api/v1/history/query", HTTP_GET, webHistoryQuery);
    server->on("/api/v1/cycles", HTTP_GET, webCycles);
    server->on("/api/v1/cycles", HTTP_POST, webCycles);
    server->on("/api/v1/web/status", HTTP_GET, webStatus);
    server->on("/api/v1/sensors", HTTP_GET, webSensors);
    server->on("/api/v1/sensors/mapping", HTTP_GET, webSensorMapping);
    server->on("/api/v1/sensors/mapping", HTTP_PUT, webSensorMapping);
    server->on("/api/v1/sensors/calibration", HTTP_POST, webSensorCalibration);
    server->on("/api/v1/sensors/capture", HTTP_GET, webAdcCapture);
    server->on("/api/v1/sensors/capture", HTTP_POST, webAdcCapture);
    server->on("/api/v1/sensors/capture", HTTP_DELETE, webAdcCapture);
    server->on("/api/v1/sensors/capture/data", HTTP_GET, webAdcCaptureData);
    server->on("/api/v1/setup", HTTP_GET, webSetup);
    server->on("/api/v1/setup", HTTP_POST, webSetup);
    server->on("/api/v1/wifi", HTTP_GET, webWifi);
    server->on("/api/v1/wifi/station", HTTP_POST, webWifiStation);
    server->on("/api/v1/wifi/ap", HTTP_POST, webWifiAp);
    server->on("/api/v1/debug", HTTP_GET, webDebug);
    server->on("/api/v1/updates", HTTP_GET, webUpdateStatus);
    server->on("/api/v1/updates/check", HTTP_POST, webUpdateCheck);
    server->on("/api/v1/updates/install", HTTP_POST, webUpdateInstall);
    server->on("/api/v1/updates/settings", HTTP_PUT, webUpdateSettings);
    server->on("/", HTTP_GET, serveWebAsset);
    server->onNotFound(serveWebAsset);
}

} // namespace web_api
