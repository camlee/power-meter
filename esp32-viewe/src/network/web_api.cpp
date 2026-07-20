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
#include "device/device_state.h"
#include "memory/heap_policy.h"
#include "network/display_web_api.h"
#include "network/history_response_encoder.h"
#include "network/http_utils.h"
#include "network/network_manager.h"
#include "network/live_websocket_service.h"
#include "network/ota_service.h"
#include "network/web_assets.generated.h"
#include "sensors/pm1_uart_protocol.h"
#include "sensors/sensor_mode.h"
#include "sensors/sensor_source_uart.h"
#include "sensors/sensors.h"
#include "time/time_service.h"

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
    switch (mode) {
        case sensor_mode::Mode::Adc: return "adc";
        case sensor_mode::Mode::Uart: return "uart";
        case sensor_mode::Mode::Demo: return "demo";
    }
    return "adc";
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
    const bool hasIn = sensors::getLatest(sensors::SENSOR_IN, in);
    const bool hasOut = sensors::getLatest(sensors::SENSOR_OUT, out);
    sensors::getLatest(sensors::SENSOR_AUX, aux);
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
    char response[1280];
    snprintf(response, sizeof(response),
             "{\"api_version\":1,\"web_build\":\"%s\",\"state_revision\":%lu,"
             "\"device_id\":\"%s\",\"hostname\":\"%s\",\"uptime_ms\":%lu,"
             "\"time_source\":\"%s\",\"date\":\"%s\",\"time\":\"%s\","
             "\"build_version\":\"%s\",\"build_date\":\"%s\",\"build_time\":\"%s\","
             "\"sensor_mode\":\"%s\",\"appearance\":{\"mode\":\"%s\",\"dark\":%s},"
             "\"data_storage_percent\":%u,\"ws_connections\":%u,\"ws_connection_limit\":%u,"
             "\"in\":{\"voltage\":%s,\"current\":%s,\"power\":%s},"
             "\"out\":{\"voltage\":%s,\"current\":%s,\"power\":%s},"
             "\"aux\":{\"power\":%s},\"net_battery_power\":%s,"
             "\"network\":{\"state\":%u,\"station_ip\":\"%s\",\"ap_ip\":\"%s\"}}",
             web_assets::kBuildId, static_cast<unsigned long>(device_state::revision()),
             device_identity::getDeviceId(), network_manager::getHostname(), static_cast<unsigned long>(millis()),
             timeSource, date, clock, BUILD_VERSION, BUILD_DATE, BUILD_TIME,
             sensorModeName(sensor_mode::get()), display_web_api::appearanceModeName(),
             display_web_api::isDark() ? "true" : "false", storagePercent,
             static_cast<unsigned>(live_websocket_service::clientCount()),
             static_cast<unsigned>(live_websocket_service::clientLimit()),
             inVoltage, inCurrent, inPower, outVoltage, outCurrent, outPower,
             auxPower, netPower, static_cast<unsigned>(network_manager::getState()),
             network_manager::getStaIpAddress(),
             network_manager::isApEnabled() ? network_manager::getApIpAddress() : "Off");
    server->sendHeader("Cache-Control", "no-store");
    server->send(200, "application/json", response);
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

void appendSensorJson(String& json, const char* id, const char* label, sensors::SensorId sensor) {
    sensors::Reading reading{};
    const bool hasReading = sensors::getLatest(sensor, reading);
    const bool observed = hasReading &&
        (reading.state == sensors::ReadingState::Valid || reading.state == sensors::ReadingState::OutOfRange);
    json += "{\"id\":\"";
    json += id;
    json += "\",\"label\":\"";
    json += label;
    json += "\",\"configured\":";
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
    json += ",\"duty\":{\"state\":\"";
    json += (hasReading ? dutyStateName(reading.dutyState) : "not_reported");
    json += "\",\"value\":";
    appendJsonFloat(json, hasReading ? reading.dutyCycle : NAN);
    json += "}}";
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
    json.reserve(2300);
    json = "{\"api_version\":1,\"source\":{\"mode\":\"";
    switch (mode) {
        case sensor_mode::Mode::Adc: json += "adc"; break;
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
    } else {
        json += "null";
    }
    json += "},\"channels\":[";
    appendSensorJson(json, "in", "In", sensors::SENSOR_IN);
    json += ',';
    appendSensorJson(json, "out", "Out", sensors::SENSOR_OUT);
    json += ',';
    appendSensorJson(json, "aux", "Aux", sensors::SENSOR_AUX);
    json += "]}";
    server->sendHeader("Cache-Control", "no-store");
    server->send(200, "application/json", json);
}

// Browser history is an explicitly requested, bounded background job. The
// query service owns LittleFS work. This handler leases its completed result
// and serializes one bounded record at a time, avoiding two full-size copies.
void webHistoryQuery() {
    if (!server->hasArg("job")) {
        bool calendar = true;
        historical_storage::CalendarRange range = historical_storage::CalendarRange::Today;
        uint32_t lookbackMinutes = 0;
        uint16_t defaultBucketMinutes = 30;
        const String rangeArg = server->arg("range");
        if (rangeArg == "last1hour") { calendar = false; lookbackMinutes = 60; defaultBucketMinutes = 2; }
        else if (rangeArg == "last6hours") { calendar = false; lookbackMinutes = 360; defaultBucketMinutes = 15; }
        else if (rangeArg == "last24hours") { calendar = false; lookbackMinutes = 1440; defaultBucketMinutes = 30; }
        else if (rangeArg == "yesterday") range = historical_storage::CalendarRange::Yesterday;
        else if (rangeArg == "last2days") range = historical_storage::CalendarRange::Last2Days;
        else if (rangeArg == "lastweek") range = historical_storage::CalendarRange::LastWeek;
        else if (rangeArg == "lasttwoweeks") range = historical_storage::CalendarRange::LastTwoWeeks;
        else if (rangeArg == "all") { range = historical_storage::CalendarRange::All; defaultBucketMinutes = 0; }
        else if (!rangeArg.isEmpty() && rangeArg != "today") {
            server->send(400, "application/json", "{\"error\":\"invalid history range\"}");
            return;
        }
        uint16_t bucketMinutes = defaultBucketMinutes;
        if (server->hasArg("bucket_minutes")) {
            const int requested = server->arg("bucket_minutes").toInt();
            bucketMinutes = range == historical_storage::CalendarRange::All && requested == 0
                ? 0 : constrain(requested, 1, 1440);
        }
        const uint32_t job = history_query_service::requestUsage({calendar,
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
            display_web_api::appearanceModeName() + "\"}";
        server->sendHeader("Cache-Control", "no-store");
        server->send(200, "application/json", response);
        return;
    }

    const String body = server->arg("plain");
    String hostname, requestedSensorMode, requestedAppearance;
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
    if (!device_identity::isValidDeviceId(hostname.c_str())) {
        server->send(400, "application/json", "{\"error\":\"Use lowercase letters, numbers, and hyphens (max 31); it cannot start or end with a hyphen.\"}");
        return;
    }

    sensor_mode::Mode mode;
    if (requestedSensorMode == "adc") mode = sensor_mode::Mode::Adc;
    else if (requestedSensorMode == "uart") mode = sensor_mode::Mode::Uart;
    else if (requestedSensorMode == "demo") mode = sensor_mode::Mode::Demo;
    else {
        server->send(400, "application/json", "{\"error\":\"invalid sensor mode\"}");
        return;
    }
    if (!display_web_api::isValidAppearance(requestedAppearance)) {
        server->send(400, "application/json", "{\"error\":\"invalid device appearance\"}");
        return;
    }

    if (resetSetup) {
        if (!clearPreferences("device") || !clearPreferences("sensors") ||
            !clearPreferences("appearance")) {
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

    String response;
    response.reserve(1050);
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
        "},\"storage\":{\"mounted\":" + (storageTotal ? "true" : "false") +
        ",\"used_kb\":" + (LittleFS.usedBytes() / 1024) + ",\"total_kb\":" +
        (storageTotal / 1024) + "},\"ota\":{\"health\":\"" + ota_service::healthStatus() +
        "\",\"validation_remaining_ms\":" + validationRemaining + ",\"running_slot\":\"" +
        ota_service::runningPartitionLabel() + "\",\"boot_slot\":\"" + ota_service::bootPartitionLabel() +
        "\",\"image_state\":\"" + ota_service::runningImageState() + "\",\"rollback_detected\":" +
        (ota_service::rollbackDetected() ? "true" : "false") + "},\"history_query\":{\"last_duration_ms\":" +
        timing.lastDurationMs + ",\"max_duration_ms\":" + timing.maxDurationMs +
        ",\"records_read\":" + timing.lastRecordsRead + ",\"files_read\":" + timing.lastFilesRead +
        ",\"was_usage\":" + (timing.lastWasUsage ? "true" : "false") + "}}";
    server->sendHeader("Cache-Control", "no-store");
    server->send(200, "application/json", response);
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

void historyFiles() {
    if (!http_utils::authorised(*server)) {
        server->send(401, "application/json", "{\"error\":\"unauthorised\"}");
        return;
    }
    size_t offset = 0;
    size_t limit = 25;
    if (server->hasArg("offset")) offset = static_cast<size_t>(server->arg("offset").toInt());
    if (server->hasArg("limit")) limit = static_cast<size_t>(server->arg("limit").toInt());
    if (limit == 0 || limit > 50) limit = 25;
    auto* files = static_cast<historical_storage::HistoryFileInfo*>(
        heap_policy::callocPreferred(limit, sizeof(historical_storage::HistoryFileInfo)));
    if (!files) {
        server->send(503, "application/json", "{\"error\":\"history response buffer unavailable\"}");
        return;
    }
    size_t total = 0;
    historical_storage::StorageStats stats{};
    const size_t count = historical_storage::listFiles(files, limit, offset, &total, &stats);

    String json;
    json.reserve(3072);
    char value[512];
    snprintf(value, sizeof(value),
             "{\"version\":1,\"flush_interval_minutes\":%u,\"record_size_bytes\":%u,"
             "\"records_per_segment\":%u,\"max_files\":%u,\"file_count\":%u,"
             "\"committed_records\":%lu,\"buffered_records\":%u,"
             "\"committed_bytes\":%lu,\"buffered_bytes\":%lu,"
             "\"offset\":%u,\"limit\":%u,\"total\":%u,\"files\":[",
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
    server->on("/api/v1/setup", HTTP_GET, webSetup);
    server->on("/api/v1/setup", HTTP_POST, webSetup);
    server->on("/api/v1/debug", HTTP_GET, webDebug);
    server->on("/", HTTP_GET, serveWebAsset);
    server->onNotFound(serveWebAsset);
}

} // namespace web_api
