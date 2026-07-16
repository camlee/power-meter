#include "ota_service.h"

#include <Arduino.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <Preferences.h>
#include <WebServer.h>
#include <Update.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_system.h>
#include <esp_heap_caps.h>
#include <mbedtls/base64.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>
#include <math.h>
#include <time.h>

// These definitions are deliberately supplied by the local build environment,
// never committed credentials.  An empty value disables firmware updates.
#include "ota_public_key.h" // generated locally from keys/ota_signing_public.pem
#include "build_time.h"
#include "lvgl_v8_port.h"
#include "data/historical_storage.h"
#include "data/energy_cycle.h"
#include "data/history_query_service.h"
#include "device/device_identity.h"
#include "device/device_state.h"
#include "network/network_manager.h"
#include "network/live_websocket_service.h"
#include "network/web_assets.generated.h"
#include "sensors/pm1_uart_protocol.h"
#include "sensors/sensor_mode.h"
#include "sensors/sensor_source_uart.h"
#include "sensors/sensors.h"
#include "time/time_service.h"
#include "ui/input/remote_input.h"
#include "ui/theme/ui_theme.h"

#ifndef OTA_SHARED_TOKEN
#define OTA_SHARED_TOKEN ""
#endif
#ifndef OTA_SIGNING_PUBLIC_KEY_PEM
#define OTA_SIGNING_PUBLIC_KEY_PEM ""
#endif
#ifndef OTA_BOARD_ID
#define OTA_BOARD_ID "meter"
#endif
#ifndef OTA_FIRMWARE_VERSION
#define OTA_FIRMWARE_VERSION "dev"
#endif

namespace ota_service {
namespace {

WebServer server(80);
constexpr uint32_t kScreenshotMinIntervalMs = 250;
uint32_t lastScreenshotMs = 0;
bool running = false;
bool uploadAccepted = false;
String uploadError;
String expectedSha256;
String expectedVersion;
uint32_t expectedImageSize = 0;
uint32_t receivedImageSize = 0;
mbedtls_sha256_context shaContext;
bool shaStarted = false;
bool liveServicePausedForUpload = false;
constexpr uint32_t kValidationWindowMs = 10000;
bool applicationReady = false;
bool healthyLoopSeen = false;
bool pendingVerify = false;
bool imageConfirmed = false;
bool didRollback = false;
uint32_t validationStartedAt = 0;
const esp_partition_t* runningPartition = nullptr;
const esp_partition_t* bootPartition = nullptr;
esp_ota_img_states_t runningState = ESP_OTA_IMG_UNDEFINED;

const char* imageStateName(esp_ota_img_states_t state) {
    switch (state) {
        case ESP_OTA_IMG_NEW: return "new";
        case ESP_OTA_IMG_PENDING_VERIFY: return "pending_verify";
        case ESP_OTA_IMG_VALID: return "valid";
        case ESP_OTA_IMG_INVALID: return "invalid";
        case ESP_OTA_IMG_ABORTED: return "aborted";
        default: return "undefined";
    }
}

const char* partitionLabel(const esp_partition_t* partition) {
    return partition ? partition->label : "unknown";
}

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

const char* appearanceModeName(ui_theme::Mode mode) {
    switch (mode) {
        case ui_theme::Mode::Light: return "light";
        case ui_theme::Mode::Dark: return "dark";
        case ui_theme::Mode::Auto: return "auto";
    }
    return "auto";
}

void persistPendingAttempt(const char* targetPartition, const String& targetVersion) {
    Preferences prefs;
    if (!prefs.begin("ota_diag", false)) return;
    prefs.putString("target_slot", targetPartition);
    prefs.putString("target_ver", targetVersion);
    prefs.putString("result", "pending");
    prefs.end();
}

void recordBootOutcome() {
    Preferences prefs;
    if (!prefs.begin("ota_diag", false)) return;
    const String target = prefs.getString("target_slot", "");
    const String result = prefs.getString("result", "");
    if (result == "pending" && !target.isEmpty() && target != partitionLabel(runningPartition)) {
        didRollback = true;
        prefs.putString("result", "rolled_back");
    } else if (result == "rolled_back") {
        didRollback = true;
    } else if (result == "confirmed") {
        didRollback = false;
    }
    prefs.end();
}

void recordConfirmation() {
    Preferences prefs;
    if (!prefs.begin("ota_diag", false)) return;
    prefs.putString("result", "confirmed");
    prefs.putString("confirmed_slot", partitionLabel(runningPartition));
    prefs.putString("confirmed_ver", OTA_FIRMWARE_VERSION);
    prefs.end();
}

bool constantTimeEqual(const String& supplied, const char* expected) {
    const size_t expectedLength = strlen(expected);
    if (supplied.length() != expectedLength) return false;
    uint8_t different = 0;
    for (size_t i = 0; i < expectedLength; ++i) different |= supplied[i] ^ expected[i];
    return different == 0;
}

bool authorised() {
    const char* token = OTA_SHARED_TOKEN;
    const String header = server.header("Authorization");
    constexpr char prefix[] = "Bearer ";
    return token[0] != '\0' && header.startsWith(prefix) &&
           constantTimeEqual(header.substring(sizeof(prefix) - 1), token);
}

void putLe16(uint8_t* dest, uint16_t value);
void putLe32(uint8_t* dest, uint32_t value);
bool writeScreenshot(WiFiClient& client, const uint8_t* data, size_t length);

void serveWebAsset() {
    const String requestPath = server.uri();
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
    if (!selected) { server.send(404, "application/json", "{\"error\":\"not found\"}"); return; }

    const String etag = String("\"") + selected->etag + "\"";
    server.sendHeader("ETag", etag);
    server.sendHeader("Content-Encoding", "gzip");
    server.sendHeader("Cache-Control", selected->immutable
        ? "public, max-age=31536000, immutable"
        : "no-cache, max-age=0, must-revalidate");
    if (server.header("If-None-Match") == etag) {
        server.send(304, selected->contentType, "");
        return;
    }
    server.send_P(200, selected->contentType,
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
             sensorModeName(sensor_mode::get()), appearanceModeName(ui_theme::mode()),
             ui_theme::isDark() ? "true" : "false", storagePercent,
             static_cast<unsigned>(live_websocket_service::clientCount()),
             static_cast<unsigned>(live_websocket_service::clientLimit()),
             inVoltage, inCurrent, inPower, outVoltage, outCurrent, outPower,
             auxPower, netPower, static_cast<unsigned>(network_manager::getState()),
             network_manager::getStaIpAddress(),
             network_manager::isApEnabled() ? network_manager::getApIpAddress() : "Off");
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", response);
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
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", json);
}

void putLeFloat(uint8_t* dest, float value) {
    uint32_t bits = 0;
    memcpy(&bits, &value, sizeof(bits));
    putLe32(dest, bits);
}

void putLeDouble(uint8_t* dest, double value) {
    uint64_t bits = 0;
    memcpy(&bits, &value, sizeof(bits));
    for (uint8_t i = 0; i < 8; ++i) dest[i] = static_cast<uint8_t>(bits >> (i * 8));
}

// Browser history is an explicitly requested, bounded background job. The
// query service owns LittleFS work; this handler only consumes its completed
// PSRAM result and serializes a compact public record format.
void webHistoryQuery() {
    if (!server.hasArg("job")) {
        bool calendar = true;
        historical_storage::CalendarRange range = historical_storage::CalendarRange::Today;
        uint32_t lookbackMinutes = 0;
        uint16_t defaultBucketMinutes = 30;
        const String rangeArg = server.arg("range");
        if (rangeArg == "last1hour") { calendar = false; lookbackMinutes = 60; defaultBucketMinutes = 2; }
        else if (rangeArg == "last6hours") { calendar = false; lookbackMinutes = 360; defaultBucketMinutes = 15; }
        else if (rangeArg == "last24hours") { calendar = false; lookbackMinutes = 1440; defaultBucketMinutes = 30; }
        else if (rangeArg == "yesterday") range = historical_storage::CalendarRange::Yesterday;
        else if (rangeArg == "last2days") range = historical_storage::CalendarRange::Last2Days;
        else if (rangeArg == "lastweek") range = historical_storage::CalendarRange::LastWeek;
        else if (rangeArg == "lasttwoweeks") range = historical_storage::CalendarRange::LastTwoWeeks;
        else if (rangeArg == "all") { range = historical_storage::CalendarRange::All; defaultBucketMinutes = 0; }
        else if (!rangeArg.isEmpty() && rangeArg != "today") {
            server.send(400, "application/json", "{\"error\":\"invalid history range\"}");
            return;
        }
        uint16_t bucketMinutes = defaultBucketMinutes;
        if (server.hasArg("bucket_minutes")) {
            const int requested = server.arg("bucket_minutes").toInt();
            bucketMinutes = range == historical_storage::CalendarRange::All && requested == 0
                ? 0 : constrain(requested, 1, 1440);
        }
        const uint32_t job = history_query_service::requestUsage({calendar,
            range, lookbackMinutes, bucketMinutes});
        if (!job) { server.send(503, "application/json", "{\"error\":\"history worker unavailable\"}"); return; }
        server.sendHeader("Cache-Control", "no-store");
        server.send(202, "application/json", String("{\"job\":") + job + "}");
        return;
    }

    const uint32_t job = static_cast<uint32_t>(server.arg("job").toInt());
    auto* buckets = static_cast<historical_storage::PowerBucket*>(heap_caps_calloc(
        history_query_service::kMaxUsageBuckets, sizeof(historical_storage::PowerBucket),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!buckets) { server.send(503, "application/json", "{\"error\":\"history response buffer unavailable\"}"); return; }
    size_t count = 0;
    historical_storage::QueryStatus status{};
    if (!history_query_service::takeUsage(job, buckets, history_query_service::kMaxUsageBuckets, count, status)) {
        heap_caps_free(buckets);
        server.sendHeader("Cache-Control", "no-store");
        server.send(202, "application/json", "{\"state\":\"pending\"}");
        return;
    }

    constexpr size_t kHeaderBytes = 32;
    constexpr size_t kRecordBytes = 80;
    const size_t responseBytes = kHeaderBytes + count * kRecordBytes;
    auto* response = static_cast<uint8_t*>(heap_caps_calloc(1, responseBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!response) { heap_caps_free(buckets); server.send(503, "application/json", "{\"error\":\"history serialization unavailable\"}"); return; }
    putLe32(response, 0x32485056); // "VPH2"; VPH1 was the incompatible alpha layout.
    response[4] = 2; response[5] = 2;
    uint16_t flags = (status.incomplete ? 1 : 0) | (status.hasInferredTime ? 2 : 0);
    putLe16(response + 6, flags);
    putLe16(response + 8, static_cast<uint16_t>(count));
    putLe16(response + 10, kRecordBytes);
    putLe32(response + 12, job);
    putLeDouble(response + 16, static_cast<double>(status.startUnixMs));
    putLeDouble(response + 24, static_cast<double>(status.endUnixMs));
    for (size_t i = 0; i < count; ++i) {
        uint8_t* record = response + kHeaderBytes + i * kRecordBytes;
        const auto& bucket = buckets[i];
        putLeDouble(record, static_cast<double>(bucket.startUnixMs));
        putLe32(record + 8, bucket.coveredMs);
        record[12] = bucket.configuredChannelMask;
        record[13] = bucket.timeFlags;
        record[14] = bucket.qualityFlags;
        for (size_t value = 0; value < historical_storage::kSensorCount; ++value) {
            putLeFloat(record + 16 + value * 4, bucket.energyWh[value]);
            putLe32(record + 48 + value * 4, bucket.channelCoverageMs[value]);
        }
        for (size_t value = 0; value < historical_storage::COMPONENT_COUNT; ++value) {
            putLeFloat(record + 28 + value * 4, bucket.componentEnergyWh[value]);
            putLe32(record + 60 + value * 4, bucket.componentCoverageMs[value]);
        }
    }
    heap_caps_free(buckets);
    server.sendHeader("Cache-Control", "no-store");
    server.setContentLength(responseBytes);
    server.send(200, "application/vnd.viewe.history-v1", "");
    WiFiClient client = server.client();
    client.setConnectionTimeout(1500);
    writeScreenshot(client, response, responseBytes);
    heap_caps_free(response);
}

bool jsonString(const String& json, const char* name, String& value) {
    String key = String("\"") + name + "\"";
    int keyAt = json.indexOf(key);
    if (keyAt < 0) return false;
    int colon = json.indexOf(':', keyAt + key.length());
    if (colon < 0) return false;
    int firstQuote = json.indexOf('\"', colon + 1);
    if (firstQuote < 0) return false;
    int lastQuote = json.indexOf('\"', firstQuote + 1);
    if (lastQuote < 0) return false;
    value = json.substring(firstQuote + 1, lastQuote);
    return true;
}

bool validSha256(const String& hash) {
    if (hash.length() != 64) return false;
    for (size_t i = 0; i < hash.length(); ++i) {
        char c = hash[i];
        if (!isxdigit(static_cast<unsigned char>(c))) return false;
    }
    return true;
}

bool jsonUnsigned(const String& json, const char* name, uint32_t& value) {
    String key = String("\"") + name + "\"";
    int keyAt = json.indexOf(key);
    if (keyAt < 0) return false;
    int colon = json.indexOf(':', keyAt + key.length());
    if (colon < 0) return false;
    int start = colon + 1;
    while (start < static_cast<int>(json.length()) &&
           isspace(static_cast<unsigned char>(json[start]))) ++start;
    int end = start;
    while (end < static_cast<int>(json.length()) && isdigit(static_cast<unsigned char>(json[end]))) ++end;
    if (end == start || (end - start) > 10) return false;
    for (int i = start; i < end; ++i) value = value * 10 + (json[i] - '0');
    return true;
}

// Parses a JSON integer without routing it through floating point. Browser
// epoch milliseconds already exceed 32-bit range, and accepting a decimal or
// exponent here would make the time contribution ambiguous.
bool jsonInteger64(const String& json, const char* name, int64_t& value) {
    const String key = String("\"") + name + "\"";
    const int keyAt = json.indexOf(key);
    if (keyAt < 0) return false;
    const int colon = json.indexOf(':', keyAt + key.length());
    if (colon < 0) return false;

    int cursor = colon + 1;
    while (cursor < static_cast<int>(json.length()) &&
           isspace(static_cast<unsigned char>(json[cursor]))) ++cursor;
    bool negative = false;
    if (cursor < static_cast<int>(json.length()) && json[cursor] == '-') {
        negative = true;
        ++cursor;
    }
    const int firstDigit = cursor;
    uint64_t magnitude = 0;
    const uint64_t limit = negative ? (uint64_t{1} << 63) : INT64_MAX;
    while (cursor < static_cast<int>(json.length()) &&
           isdigit(static_cast<unsigned char>(json[cursor]))) {
        const uint8_t digit = json[cursor++] - '0';
        if (magnitude > (limit - digit) / 10) return false;
        magnitude = magnitude * 10 + digit;
    }
    if (cursor == firstDigit) return false;
    while (cursor < static_cast<int>(json.length()) &&
           isspace(static_cast<unsigned char>(json[cursor]))) ++cursor;
    if (cursor < static_cast<int>(json.length()) && json[cursor] != ',' && json[cursor] != '}') return false;

    if (negative) {
        value = magnitude == (uint64_t{1} << 63) ? INT64_MIN : -static_cast<int64_t>(magnitude);
    } else {
        value = static_cast<int64_t>(magnitude);
    }
    return true;
}

bool jsonBool(const String& json, const char* name, bool& value) {
    String key = String("\"") + name + "\"";
    int keyAt = json.indexOf(key);
    if (keyAt < 0) return false;
    int colon = json.indexOf(':', keyAt + key.length());
    if (colon < 0) return false;
    int valueAt = colon + 1;
    while (valueAt < static_cast<int>(json.length()) &&
           isspace(static_cast<unsigned char>(json[valueAt]))) ++valueAt;
    const String tail = json.substring(valueAt);
    if (tail.startsWith("true")) { value = true; return true; }
    if (tail.startsWith("false")) { value = false; return true; }
    return false;
}

bool clearPreferences(const char* name) {
    Preferences prefs;
    if (!prefs.begin(name, false)) return false;
    const bool cleared = prefs.clear();
    prefs.end();
    return cleared;
}

void webSetup() {
    if (server.method() == HTTP_GET) {
        String response = String("{\"api_version\":1,\"hostname\":\"") +
            device_identity::getDeviceId() + "\",\"sensor_mode\":\"" +
            sensorModeName(sensor_mode::get()) + "\",\"appearance\":\"" +
            appearanceModeName(ui_theme::mode()) + "\"}";
        server.sendHeader("Cache-Control", "no-store");
        server.send(200, "application/json", response);
        return;
    }

    const String body = server.arg("plain");
    String hostname, requestedSensorMode, requestedAppearance;
    bool resetSetup = false, resetWifi = false, resetCalibration = false, resetUsage = false;
    if (!jsonString(body, "hostname", hostname) ||
        !jsonString(body, "sensor_mode", requestedSensorMode) ||
        !jsonString(body, "appearance", requestedAppearance) ||
        !jsonBool(body, "reset_setup", resetSetup) ||
        !jsonBool(body, "reset_wifi", resetWifi) ||
        !jsonBool(body, "reset_calibration", resetCalibration) ||
        !jsonBool(body, "reset_usage", resetUsage)) {
        server.send(400, "application/json", "{\"error\":\"invalid setup request\"}");
        return;
    }
    if (!device_identity::isValidDeviceId(hostname.c_str())) {
        server.send(400, "application/json", "{\"error\":\"Use lowercase letters, numbers, and hyphens (max 31); it cannot start or end with a hyphen.\"}");
        return;
    }

    sensor_mode::Mode mode;
    if (requestedSensorMode == "adc") mode = sensor_mode::Mode::Adc;
    else if (requestedSensorMode == "uart") mode = sensor_mode::Mode::Uart;
    else if (requestedSensorMode == "demo") mode = sensor_mode::Mode::Demo;
    else {
        server.send(400, "application/json", "{\"error\":\"invalid sensor mode\"}");
        return;
    }
    ui_theme::Mode appearance;
    if (requestedAppearance == "light") appearance = ui_theme::Mode::Light;
    else if (requestedAppearance == "dark") appearance = ui_theme::Mode::Dark;
    else if (requestedAppearance == "auto") appearance = ui_theme::Mode::Auto;
    else {
        server.send(400, "application/json", "{\"error\":\"invalid device appearance\"}");
        return;
    }

    if (resetSetup) {
        if (!clearPreferences("device") || !clearPreferences("sensors") ||
            !clearPreferences("appearance")) {
            server.send(500, "application/json", "{\"error\":\"Could not reset setup preferences.\"}");
            return;
        }
    } else {
        if (hostname != device_identity::getDeviceId() &&
            !device_identity::setDeviceId(hostname.c_str())) {
            server.send(500, "application/json", "{\"error\":\"Could not save hostname.\"}");
            return;
        }
        if (mode != sensor_mode::get() && !sensor_mode::set(mode)) {
            server.send(500, "application/json", "{\"error\":\"Could not save sensor mode.\"}");
            return;
        }
        if (appearance != ui_theme::mode()) ui_theme::setMode(appearance);
    }
    if (resetCalibration && !clearPreferences("sensor_cal")) {
        server.send(500, "application/json", "{\"error\":\"Could not reset sensor calibration.\"}");
        return;
    }
    if (resetWifi && !network_manager::clearSavedCredentials()) {
        server.send(500, "application/json", "{\"error\":\"Could not reset Wi-Fi credentials.\"}");
        return;
    }
    if (resetUsage && !historical_storage::clearAll()) {
        server.send(500, "application/json", "{\"error\":\"Could not reset usage history.\"}");
        return;
    }

    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", "{\"ok\":true,\"restarting\":true}");
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
    const uint32_t validationRemaining = validationRemainingMs();
    history_query_service::Timing timing{};
    history_query_service::getTiming(timing);

    String response;
    response.reserve(1050);
    response = String("{\"api_version\":1,\"lvgl\":\"") + LVGL_VERSION_MAJOR + "." +
        LVGL_VERSION_MINOR + "." + LVGL_VERSION_PATCH + "\",\"sdk\":\"" + ESP.getSdkVersion() +
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
        (storageTotal / 1024) + "},\"ota\":{\"health\":\"" + healthStatus() +
        "\",\"validation_remaining_ms\":" + validationRemaining + ",\"running_slot\":\"" +
        runningPartitionLabel() + "\",\"boot_slot\":\"" + bootPartitionLabel() +
        "\",\"image_state\":\"" + runningImageState() + "\",\"rollback_detected\":" +
        (rollbackDetected() ? "true" : "false") + "},\"history_query\":{\"last_duration_ms\":" +
        timing.lastDurationMs + ",\"max_duration_ms\":" + timing.maxDurationMs +
        ",\"records_read\":" + timing.lastRecordsRead + ",\"files_read\":" + timing.lastFilesRead +
        ",\"was_usage\":" + (timing.lastWasUsage ? "true" : "false") + "}}";
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", response);
}

void putLe16(uint8_t* dest, uint16_t value) {
    dest[0] = value & 0xff;
    dest[1] = value >> 8;
}

void putLe32(uint8_t* dest, uint32_t value) {
    dest[0] = value & 0xff;
    dest[1] = (value >> 8) & 0xff;
    dest[2] = (value >> 16) & 0xff;
    dest[3] = (value >> 24) & 0xff;
}

// Keep a slow or vanished remote viewer from monopolising the synchronous
// WebServer. NetworkClient::write can return a short write under congestion;
// make progress in small chunks and abandon a transfer that stops progressing.
bool writeScreenshot(WiFiClient& client, const uint8_t* data, size_t length) {
    constexpr size_t kChunkBytes = 1460; // one Ethernet/TCP payload
    constexpr uint32_t kStallTimeoutMs = 1500;
    uint32_t lastProgress = millis();
    while (length > 0 && client.connected()) {
        const size_t requested = min(length, kChunkBytes);
        const size_t written = client.write(data, requested);
        if (written > 0) {
            data += written;
            length -= written;
            lastProgress = millis();
        } else if (millis() - lastProgress >= kStallTimeoutMs) {
            client.stop();
            return false;
        }
        delay(0);
    }
    return length == 0;
}

// The RGB panel retains the rendered image in its framebuffer, making this a
// faithful capture of the physical display rather than a second UI renderer.
void screenshot() {
    const uint32_t now = millis();
    if (now - lastScreenshotMs < kScreenshotMinIntervalMs) {
        server.sendHeader("Retry-After", "1");
        server.send(429, "application/json", "{\"error\":\"screenshot rate limited\"}");
        return;
    }
    lastScreenshotMs = now;
    uint16_t width = 0, height = 0;
    const uint8_t* frameBuffer = lvgl_port_get_remote_framebuffer(&width, &height);
    if (!frameBuffer) {
        server.send(503, "application/json", "{\"error\":\"framebuffer unavailable\"}");
        return;
    }

    uint16_t scale = 1;
    if (server.hasArg("scale")) {
        const int requestedScale = server.arg("scale").toInt();
        if (requestedScale == 2 || requestedScale == 4) scale = requestedScale;
    }
    const uint16_t outputWidth = width / scale;
    const uint16_t outputHeight = height / scale;

    // PPM is useful for CLI automation and is directly readable by common
    // image tooling without an encoder. The browser viewer uses BMP below.
    if (server.arg("format") == "ppm") {
        const String ppmHeader = String("P6\n") + outputWidth + " " + outputHeight + "\n255\n";
        server.setContentLength(ppmHeader.length() + static_cast<size_t>(outputWidth) * outputHeight * 3);
        server.send(200, "image/x-portable-pixmap", "");
        WiFiClient client = server.client();
        client.setConnectionTimeout(1500);
        if (!writeScreenshot(client, reinterpret_cast<const uint8_t*>(ppmHeader.c_str()), ppmHeader.length())) return;
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
            if (!writeScreenshot(client, line, outputWidth * 3)) return;
        }
        return;
    }

    // 24-bit BMP is universally displayable in browsers and requires no JPEG
    // encoder or a second full-size output buffer on the ESP32.
    constexpr size_t kHeaderBytes = 54;
    const uint32_t pixelBytes = static_cast<uint32_t>(outputWidth) * outputHeight * 3;
    const size_t responseBytes = kHeaderBytes + pixelBytes;
    uint8_t* response = static_cast<uint8_t*>(heap_caps_malloc(responseBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!response) {
        server.send(503, "application/json", "{\"error\":\"screenshot buffer unavailable\"}");
        return;
    }
    uint8_t* header = response;
    memset(header, 0, kHeaderBytes);
    header[0] = 'B'; header[1] = 'M';
    putLe32(header + 2, kHeaderBytes + pixelBytes);
    putLe32(header + 10, kHeaderBytes);
    putLe32(header + 14, 40);
    putLe32(header + 18, outputWidth);
    putLe32(header + 22, outputHeight);
    putLe16(header + 26, 1);
    putLe16(header + 28, 24);
    putLe32(header + 34, pixelBytes);

    // Finish the conversion before opening the response. Interleaving tiny,
    // random PSRAM framebuffer reads with TCP writes became extremely slow
    // while LVGL was actively redrawing the calibration chart. A contiguous
    // encoded snapshot gives the network stack a stable sequential buffer.
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
                const uint8_t r = ((rgb565 >> 11) & 0x1f) * 255 / 31;
                const uint8_t g = ((rgb565 >> 5) & 0x3f) * 255 / 63;
                const uint8_t b = (rgb565 & 0x1f) * 255 / 31;
                *output++ = b;
                *output++ = g;
                *output++ = r;
        }
    }
    if (displayLocked) lvgl_port_unlock();

    server.setContentLength(responseBytes);
    server.send(200, "image/bmp", "");
    WiFiClient client = server.client();
    client.setConnectionTimeout(1500);
    writeScreenshot(client, response, responseBytes);
    heap_caps_free(response);
}

bool parseRemotePoint(uint16_t& x, uint16_t& y, bool* pressed = nullptr) {
    uint32_t parsedX = 0, parsedY = 0;
    const String body = server.arg("plain");
    if (!jsonUnsigned(body, "x", parsedX) || !jsonUnsigned(body, "y", parsedY) || parsedX > 319 || parsedY > 479) {
        server.send(400, "application/json", "{\"error\":\"x and y must be display coordinates\"}");
        return false;
    }
    if (pressed && !jsonBool(body, "pressed", *pressed)) {
        server.send(400, "application/json", "{\"error\":\"pressed must be boolean\"}");
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
    server.send(200, "application/json", "{\"ok\":true}");
}

void remotePointer() {
    uint16_t x, y;
    bool pressed = false;
    if (!parseRemotePoint(x, y, &pressed)) return;
    remote_input::setPointer(x, y, pressed);
    server.send(200, "application/json", "{\"ok\":true}");
}

void browserTimeAnchor() {
    const String body = server.arg("plain");
    int64_t unixMs = 0;
    // Keep obviously bad browser clocks out of persisted history. This is a
    // sanity bound rather than a promise about the product's supported life.
    constexpr int64_t kEarliestPlausibleUnixMs = 1577836800000LL; // 2020-01-01 UTC
    constexpr int64_t kLatestPlausibleUnixMs = 4102444800000LL;   // 2100-01-01 UTC
    if (!jsonInteger64(body, "unix_ms", unixMs) ||
        unixMs < kEarliestPlausibleUnixMs || unixMs >= kLatestPlausibleUnixMs) {
        server.send(400, "application/json",
                    "{\"ok\":false,\"error\":\"unix_ms must be an integer browser epoch in milliseconds between 2020 and 2100\"}");
        return;
    }

    int64_t parsedOffset = time_service::utcOffsetMinutes();
    const bool offsetProvided = body.indexOf("\"utc_offset_minutes\"") >= 0;
    if (offsetProvided &&
        (!jsonInteger64(body, "utc_offset_minutes", parsedOffset) || parsedOffset < -840 || parsedOffset > 840)) {
        server.send(400, "application/json",
                    "{\"ok\":false,\"error\":\"utc_offset_minutes must be an integer from -840 to 840\"}");
        return;
    }

    const int16_t offsetMinutes = static_cast<int16_t>(parsedOffset);
    // A browser Date.now() sample received over the LAN is useful but not a
    // precision clock measurement. Preserve that distinction in the anchor.
    constexpr uint32_t kBrowserUncertaintyMs = 1000;
    if (!time_service::submitAnchor(unixMs, time_service::AnchorSource::Browser,
                                    offsetMinutes, kBrowserUncertaintyMs)) {
        server.send(503, "application/json",
                    "{\"ok\":false,\"error\":\"time anchor could not be persisted\"}");
        return;
    }
    device_state::changed(device_state::Domain::Time);

    server.send(200, "application/json",
                String("{\"ok\":true,\"source\":\"browser\",\"unix_ms\":") + unixMs +
                ",\"utc_offset_minutes\":" + offsetMinutes +
                ",\"offset_updated\":" + (offsetProvided ? "true" : "false") + "}");
}

void webCycles() {
    if (server.method() == HTTP_POST) {
        uint32_t hour = 0;
        if (!jsonUnsigned(server.arg("plain"), "end_hour", hour) || hour >= 24) {
            server.send(400, "application/json",
                        "{\"ok\":false,\"error\":\"end_hour must be an integer from 0 to 23\"}");
            return;
        }
        if (!energy_cycle::setEndHour(static_cast<uint8_t>(hour))) {
            server.send(503, "application/json",
                        "{\"ok\":false,\"error\":\"cycle end time could not be persisted\"}");
            return;
        }
        server.sendHeader("Cache-Control", "no-store");
        server.send(200, "application/json",
                    String("{\"ok\":true,\"end_hour\":") + hour + "}");
        return;
    }

    if (!server.hasArg("job")) {
        const uint32_t job = history_query_service::requestCycles();
        if (!job) {
            server.send(503, "application/json", "{\"error\":\"history worker unavailable\"}");
            return;
        }
        server.sendHeader("Cache-Control", "no-store");
        server.send(202, "application/json", String("{\"job\":") + job + "}");
        return;
    }

    const uint32_t job = static_cast<uint32_t>(server.arg("job").toInt());
    auto* summaries = static_cast<energy_cycle::Summary*>(heap_caps_calloc(
        energy_cycle::kRecentCycleCount, sizeof(energy_cycle::Summary),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!summaries) {
        server.send(503, "application/json", "{\"error\":\"cycle response buffer unavailable\"}");
        return;
    }
    size_t count = 0;
    if (!history_query_service::takeCycles(job, summaries, energy_cycle::kRecentCycleCount, count)) {
        heap_caps_free(summaries);
        server.sendHeader("Cache-Control", "no-store");
        server.send(202, "application/json", "{\"state\":\"pending\"}");
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
    heap_caps_free(summaries);
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", response);
}

void historyFiles() {
    if (!authorised()) {
        server.send(401, "application/json", "{\"error\":\"unauthorised\"}");
        return;
    }
    size_t offset = 0;
    size_t limit = 25;
    if (server.hasArg("offset")) offset = static_cast<size_t>(server.arg("offset").toInt());
    if (server.hasArg("limit")) limit = static_cast<size_t>(server.arg("limit").toInt());
    if (limit == 0 || limit > 50) limit = 25;
    auto* files = static_cast<historical_storage::HistoryFileInfo*>(heap_caps_calloc(
        limit, sizeof(historical_storage::HistoryFileInfo), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!files) {
        server.send(503, "application/json", "{\"error\":\"history response buffer unavailable\"}");
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
    server.send(200, "application/json", json);
    heap_caps_free(files);
}

void pauseLiveServiceForUpload() {
    if (liveServicePausedForUpload) return;
    liveServicePausedForUpload = live_websocket_service::stop();
    if (liveServicePausedForUpload) Serial.println("Live WebSocket service paused for OTA verification");
}

void resumeLiveServiceAfterUploadFailure() {
    if (!liveServicePausedForUpload) return;
    liveServicePausedForUpload = false;
    if (!live_websocket_service::begin()) {
        Serial.println("Live WebSocket service could not restart after rejected OTA");
    }
}

void setSignatureVerificationError(const char* stage, int rc) {
    uploadError = String("manifest signature verification failed at ") + stage +
        " (rc=" + rc + ", largest_internal=" +
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) + ")";
    Serial.println(uploadError);
}

// The canonical manifest bytes themselves are signed with ECDSA P-256/SHA-256.
bool verifyManifestSignature(const String& manifest, const String& signatureBase64) {
    if (strlen(OTA_SIGNING_PUBLIC_KEY_PEM) == 0) {
        uploadError = "signing public key is not configured";
        return false;
    }
    size_t signatureLength = 0;
    int rc = mbedtls_base64_decode(nullptr, 0, &signatureLength,
                                   reinterpret_cast<const unsigned char*>(signatureBase64.c_str()),
                                   signatureBase64.length());
    if (rc != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL || signatureLength == 0 || signatureLength > 80) {
        uploadError = "invalid base64 signature";
        return false;
    }
    uint8_t signature[80];
    if (mbedtls_base64_decode(signature, sizeof(signature), &signatureLength,
                              reinterpret_cast<const unsigned char*>(signatureBase64.c_str()),
                              signatureBase64.length()) != 0) {
        uploadError = "invalid base64 signature";
        return false;
    }
    uint8_t digest[32];
    if (mbedtls_sha256(reinterpret_cast<const unsigned char*>(manifest.c_str()), manifest.length(), digest, 0) != 0) {
        uploadError = "manifest hash failed";
        return false;
    }
    mbedtls_pk_context key;
    mbedtls_pk_init(&key);
    const char* failureStage = "public key parse";
    rc = mbedtls_pk_parse_public_key(&key,
            reinterpret_cast<const unsigned char*>(OTA_SIGNING_PUBLIC_KEY_PEM),
            strlen(OTA_SIGNING_PUBLIC_KEY_PEM) + 1);
    if (rc == 0 && !mbedtls_pk_can_do(&key, MBEDTLS_PK_ECDSA)) {
        failureStage = "public key type";
        rc = MBEDTLS_ERR_PK_TYPE_MISMATCH;
    }
    if (rc == 0) {
        failureStage = "ECDSA verify";
        rc = mbedtls_pk_verify(&key, MBEDTLS_MD_SHA256, digest, sizeof(digest),
                               signature, signatureLength);
    }
    mbedtls_pk_free(&key);
    if (rc != 0) {
        setSignatureVerificationError(failureStage, rc);
        return false;
    }
    return true;
}

bool prepareUpload() {
    if (!authorised()) { uploadError = "unauthorised"; return false; }
    const String manifest = server.arg("manifest");
    const String signature = server.arg("signature");
    String board, hash, version;
    uint32_t format = 0, imageSize = 0;
    if (manifest.isEmpty() || signature.isEmpty() || !jsonString(manifest, "board", board) ||
        !jsonString(manifest, "sha256", hash) || !jsonString(manifest, "version", version) ||
        !jsonUnsigned(manifest, "format", format) || !jsonUnsigned(manifest, "image_size", imageSize)) {
        uploadError = "invalid manifest";
        return false;
    }
    const String canonical = String("{\"board\":\"") + board + "\",\"format\":" + format +
        ",\"image_size\":" + imageSize + ",\"sha256\":\"" + hash +
        "\",\"version\":\"" + version + "\"}";
    if (format != 1 || manifest != canonical) { uploadError = "manifest is not canonical format 1"; return false; }
    if (board != OTA_BOARD_ID) { uploadError = "firmware board does not match"; return false; }
    if (imageSize == 0) { uploadError = "invalid manifest image_size"; return false; }
    if (!validSha256(hash)) { uploadError = "invalid manifest sha256"; return false; }
    pauseLiveServiceForUpload();
    if (!verifyManifestSignature(manifest, signature)) {
        resumeLiveServiceAfterUploadFailure();
        return false;
    }
    expectedSha256 = hash;
    expectedVersion = version;
    expectedImageSize = imageSize;
    receivedImageSize = 0;
    mbedtls_sha256_init(&shaContext);
    if (mbedtls_sha256_starts(&shaContext, 0) != 0) {
        uploadError = "image hash setup failed";
        mbedtls_sha256_free(&shaContext);
        resumeLiveServiceAfterUploadFailure();
        return false;
    }
    shaStarted = true;
    if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) {
        uploadError = String("not enough OTA space: ") + Update.errorString();
        mbedtls_sha256_free(&shaContext);
        shaStarted = false;
        resumeLiveServiceAfterUploadFailure();
        return false;
    }
    return true;
}

String hexDigest(const uint8_t* digest, size_t length) {
    static const char hex[] = "0123456789abcdef";
    String result;
    result.reserve(length * 2);
    for (size_t i = 0; i < length; ++i) { result += hex[digest[i] >> 4]; result += hex[digest[i] & 15]; }
    return result;
}

void handleUpload() {
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
        uploadAccepted = false;
        uploadError = "";
        expectedSha256 = "";
        expectedVersion = "";
        expectedImageSize = 0;
        receivedImageSize = 0;
        shaStarted = false;
        // Multipart text fields must precede the firmware file. This lets us
        // authenticate and validate the signed manifest before flash is erased.
        uploadAccepted = prepareUpload();
    } else if (upload.status == UPLOAD_FILE_WRITE && uploadAccepted) {
        if (upload.currentSize > expectedImageSize - receivedImageSize ||
            mbedtls_sha256_update(&shaContext, upload.buf, upload.currentSize) != 0 ||
            Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
            uploadError = String("flash write failed: ") + Update.errorString();
            uploadAccepted = false;
            Update.abort();
            if (shaStarted) { mbedtls_sha256_free(&shaContext); shaStarted = false; }
        } else {
            receivedImageSize += upload.currentSize;
        }
    } else if (upload.status == UPLOAD_FILE_END && uploadAccepted) {
        uint8_t digest[32];
        if (!shaStarted || receivedImageSize != expectedImageSize ||
            mbedtls_sha256_finish(&shaContext, digest) != 0 ||
            !hexDigest(digest, sizeof(digest)).equalsIgnoreCase(expectedSha256)) {
            uploadError = "image sha256 does not match manifest";
            Update.abort();
            uploadAccepted = false;
        } else if (!Update.end(true)) {
            uploadError = String("OTA finalisation failed: ") + Update.errorString();
            uploadAccepted = false;
        }
        if (shaStarted) { mbedtls_sha256_free(&shaContext); shaStarted = false; }
    } else if (upload.status == UPLOAD_FILE_ABORTED) {
        Update.abort();
        if (shaStarted) { mbedtls_sha256_free(&shaContext); shaStarted = false; }
        uploadAccepted = false;
        uploadError = "upload aborted";
        resumeLiveServiceAfterUploadFailure();
    }
}

void completeUpload() {
    if (!uploadAccepted) {
        resumeLiveServiceAfterUploadFailure();
        server.send(uploadError == "unauthorised" ? 401 : 400, "application/json",
                    String("{\"ok\":false,\"error\":\"") + uploadError + "\"}");
        return;
    }
    const esp_partition_t* target = esp_ota_get_next_update_partition(nullptr);
    if (!target) {
        resumeLiveServiceAfterUploadFailure();
        server.send(500, "application/json", "{\"ok\":false,\"error\":\"OTA target partition unavailable\"}");
        return;
    }
    // This marker is deliberately written before reboot. If the candidate
    // never reaches confirmation, the previous image can report its rollback.
    persistPendingAttempt(target->label, expectedVersion);
    server.send(200, "application/json", String("{\"ok\":true,\"rebooting\":true,\"target_partition\":\"") + target->label + "\"}");
    delay(250); // flush HTTP response before booting the selected OTA partition
    ESP.restart();
}

void info() {
    if (!authorised()) { server.send(401, "application/json", "{\"error\":\"unauthorised\"}"); return; }
    char id[20];
    snprintf(id, sizeof(id), "%012llx", ESP.getEfuseMac());
    const uint32_t remaining = validationRemainingMs();
    server.send(200, "application/json", String("{\"board\":\"") + OTA_BOARD_ID +
                "\",\"version\":\"" + OTA_FIRMWARE_VERSION + "\",\"hardware_id\":\"" + id +
                "\",\"health\":\"" + healthStatus() + "\",\"validated\":" +
                (imageConfirmed ? "true" : "false") + ",\"validation_remaining_ms\":" + remaining +
                ",\"reset_reason\":\"" + resetReasonName(esp_reset_reason()) +
                "\",\"uptime_ms\":" + millis() + ",\"running_partition\":\"" + partitionLabel(runningPartition) +
                "\",\"boot_partition\":\"" + partitionLabel(bootPartition) +
                "\",\"image_state\":\"" + imageStateName(runningState) +
                "\",\"rollback_detected\":" + (didRollback ? "true" : "false") +
                ",\"rollback_supported\":" + (esp_ota_check_rollback_is_possible() ? "true" : "false") + "}");
}

} // namespace

void begin() {
    if (running) return;
    const char* headers[] = {"X-OTA-Token", "If-None-Match"};
    server.collectHeaders(headers, 2);
    server.on("/api/v1/info", HTTP_GET, info);
    server.on("/api/v1/update", HTTP_POST, completeUpload, handleUpload);
    server.on("/api/v1/display/screenshot.bmp", HTTP_GET, screenshot);
    server.on("/api/v1/display/tap", HTTP_POST, remoteTap);
    server.on("/api/v1/display/pointer", HTTP_POST, remotePointer);
    server.on("/api/v1/time/anchor", HTTP_POST, browserTimeAnchor);
    server.on("/api/v1/history/files", HTTP_GET, historyFiles);
    server.on("/api/v1/history/query", HTTP_GET, webHistoryQuery);
    server.on("/api/v1/cycles", HTTP_GET, webCycles);
    server.on("/api/v1/cycles", HTTP_POST, webCycles);
    server.on("/api/v1/web/status", HTTP_GET, webStatus);
    server.on("/api/v1/sensors", HTTP_GET, webSensors);
    server.on("/api/v1/setup", HTTP_GET, webSetup);
    server.on("/api/v1/setup", HTTP_POST, webSetup);
    server.on("/api/v1/debug", HTTP_GET, webDebug);
    server.on("/", HTTP_GET, serveWebAsset);
    server.onNotFound(serveWebAsset);
    server.begin();
    runningPartition = esp_ota_get_running_partition();
    bootPartition = esp_ota_get_boot_partition();
    if (runningPartition && esp_ota_get_state_partition(runningPartition, &runningState) == ESP_OK) {
        pendingVerify = runningState == ESP_OTA_IMG_PENDING_VERIFY;
        imageConfirmed = !pendingVerify;
    }
    recordBootOutcome();
    running = true;
}

void update() {
    if (running) server.handleClient();
    if (!pendingVerify || !applicationReady || !healthyLoopSeen) return;
    if (millis() - validationStartedAt < kValidationWindowMs) return;
    if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
        pendingVerify = false;
        imageConfirmed = true;
        runningState = ESP_OTA_IMG_VALID;
        recordConfirmation();
        Serial.println("OTA image confirmed after stable operation");
    } else {
        Serial.println("OTA image confirmation failed; rollback remains armed");
    }
}
bool isRunning() { return running; }
void setApplicationReady() {
    applicationReady = true;
    validationStartedAt = millis();
}
void noteHealthyLoop() { healthyLoopSeen = true; }
const char* healthStatus() {
    if (didRollback) return "rolled_back";
    if (pendingVerify) return applicationReady && healthyLoopSeen ? "pending_verify" : "starting";
    return imageConfirmed ? "confirmed" : "ready";
}
const char* runningPartitionLabel() { return partitionLabel(runningPartition); }
const char* bootPartitionLabel() { return partitionLabel(bootPartition); }
const char* runningImageState() { return imageStateName(runningState); }
bool rollbackDetected() { return didRollback; }
bool rollbackSupported() { return esp_ota_check_rollback_is_possible(); }
uint32_t validationRemainingMs() {
    if (!pendingVerify || !applicationReady) return 0;
    const uint32_t elapsed = millis() - validationStartedAt;
    return elapsed < kValidationWindowMs ? kValidationWindowMs - elapsed : 0;
}

} // namespace ota_service
