#include "ota_service.h"
#include "ota_routes.h"

#include <Arduino.h>
#include <Preferences.h>
#include <Update.h>
#include <WebServer.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_system.h>
#include <mbedtls/base64.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>
#include <atomic>
#include <time.h>

#include "ota_public_key.h"
#include "network/http_utils.h"
#include "network/live_websocket_service.h"
#include "network/semver.h"
#include "network/web_server.h"

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

WebServer* server = nullptr;
bool running = false;
bool uploadAccepted = false;
String uploadError;
String expectedSha256;
String expectedVersion;
uint32_t expectedImageSize = 0;
uint32_t receivedImageSize = 0;
ManifestInfo activeManifest{};
mbedtls_sha256_context shaContext;
bool shaStarted = false;
bool liveServicePausedForUpload = false;
std::atomic<bool> writerBusy{false};
constexpr uint32_t kValidationWindowMs = 30000;
constexpr int64_t kEarliestCredibleUnixSeconds = 1577836800LL; // 2020-01-01
bool applicationReady = false;
bool healthyLoopSeen = false;
bool pendingVerify = false;
bool imageConfirmed = false;
bool didRollback = false;
uint32_t validationStartedAt = 0;
const esp_partition_t* runningPartition = nullptr;
const esp_partition_t* bootPartition = nullptr;
esp_ota_img_states_t runningState = ESP_OTA_IMG_UNDEFINED;
String rollbackVersionValue;
int64_t lastUpdateUnixSecondsValue = 0;
bool updateDateMayBackfill = false;

int64_t credibleUnixTime() {
    const int64_t now = static_cast<int64_t>(time(nullptr));
    return now >= kEarliestCredibleUnixSeconds ? now : 0;
}

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

void persistPendingAttempt(const char* targetPartition, const String& targetVersion) {
    Preferences prefs;
    if (!prefs.begin("ota_diag", false)) return;
    prefs.putString("target_slot", targetPartition);
    prefs.putString("target_ver", targetVersion);
    prefs.putLong64("attempt_at", credibleUnixTime());
    prefs.putString("result", "pending");
    prefs.end();
}

void recordBootOutcome() {
    Preferences prefs;
    if (!prefs.begin("ota_diag", false)) return;
    const String target = prefs.getString("target_slot", "");
    const String targetVersion = prefs.getString("target_ver", "");
    const String result = prefs.getString("result", "");
    if (result == "pending" && !target.isEmpty() && target != partitionLabel(runningPartition)) {
        didRollback = true;
        rollbackVersionValue = targetVersion;
        prefs.putString("result", "rolled_back");
    } else if (result == "rolled_back") {
        didRollback = true;
        rollbackVersionValue = targetVersion;
    } else if (result == "confirmed") {
        didRollback = false;
    }
    prefs.end();
}

void recordConfirmation() {
    Preferences prefs;
    if (!prefs.begin("ota_diag", false)) return;
    const int64_t attemptedAt = prefs.getLong64("attempt_at", 0);
    const int64_t confirmedAt = credibleUnixTime();
    lastUpdateUnixSecondsValue = confirmedAt ? confirmedAt : attemptedAt;
    updateDateMayBackfill = lastUpdateUnixSecondsValue == 0;
    prefs.putString("result", "confirmed");
    prefs.putString("confirmed_slot", partitionLabel(runningPartition));
    prefs.putString("confirmed_ver", OTA_FIRMWARE_VERSION);
    prefs.putLong64("confirmed_at", lastUpdateUnixSecondsValue);
    prefs.end();
}

void loadCurrentUpdateDate() {
    Preferences prefs;
    if (!prefs.begin("ota_diag", true)) return;
    const String confirmedVersion = prefs.getString("confirmed_ver", "");
    if (confirmedVersion == OTA_FIRMWARE_VERSION) {
        lastUpdateUnixSecondsValue = prefs.getLong64("confirmed_at", 0);
        updateDateMayBackfill =
            lastUpdateUnixSecondsValue == 0 &&
            prefs.getString("result", "") == "confirmed";
    }
    prefs.end();
}

void backfillCurrentUpdateDate() {
    if (!updateDateMayBackfill || lastUpdateUnixSecondsValue != 0) return;
    const int64_t now = credibleUnixTime();
    if (!now) return;
    Preferences prefs;
    if (!prefs.begin("ota_diag", false)) return;
    if (prefs.getString("result", "") == "confirmed" &&
        prefs.getString("confirmed_ver", "") == OTA_FIRMWARE_VERSION) {
        prefs.putLong64("confirmed_at", now);
        lastUpdateUnixSecondsValue = now;
        updateDateMayBackfill = false;
    }
    prefs.end();
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

bool verifyManifestSignature(const String& manifest, const String& signatureBase64,
                             String& error) {
    if (strlen(OTA_SIGNING_PUBLIC_KEY_PEM) == 0) {
        error = "signing public key is not configured";
        return false;
    }
    size_t signatureLength = 0;
    int rc = mbedtls_base64_decode(nullptr, 0, &signatureLength,
                                   reinterpret_cast<const unsigned char*>(signatureBase64.c_str()),
                                   signatureBase64.length());
    if (rc != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL || signatureLength == 0 || signatureLength > 80) {
        error = "invalid base64 signature";
        return false;
    }
    uint8_t signature[80];
    if (mbedtls_base64_decode(signature, sizeof(signature), &signatureLength,
                              reinterpret_cast<const unsigned char*>(signatureBase64.c_str()),
                              signatureBase64.length()) != 0) {
        error = "invalid base64 signature";
        return false;
    }
    uint8_t digest[32];
    if (mbedtls_sha256(reinterpret_cast<const unsigned char*>(manifest.c_str()),
                       manifest.length(), digest, 0) != 0) {
        error = "manifest hash failed";
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
        error = String("manifest signature verification failed at ") + failureStage +
            " (rc=" + rc + ")";
        Serial.println(error);
        return false;
    }
    return true;
}

bool prepareUpload() {
    const String manifest = server->arg("manifest");
    const String signature = server->arg("signature");
    return beginSignedInstall(manifest, signature, activeManifest, uploadError);
}

String hexDigest(const uint8_t* digest, size_t length) {
    static const char hex[] = "0123456789abcdef";
    String result;
    result.reserve(length * 2);
    for (size_t i = 0; i < length; ++i) {
        result += hex[digest[i] >> 4];
        result += hex[digest[i] & 15];
    }
    return result;
}

void handleUpload() {
    HTTPUpload& upload = server->upload();
    if (upload.status == UPLOAD_FILE_START) {
        uploadAccepted = false;
        uploadError = "";
        expectedSha256 = "";
        expectedVersion = "";
        expectedImageSize = 0;
        receivedImageSize = 0;
        shaStarted = false;
        uploadAccepted = prepareUpload();
    } else if (upload.status == UPLOAD_FILE_WRITE && uploadAccepted) {
        if (!writeSignedInstall(upload.buf, upload.currentSize, uploadError)) {
            uploadAccepted = false;
        }
    } else if (upload.status == UPLOAD_FILE_END && uploadAccepted) {
        uploadAccepted = finishSignedInstall(uploadError);
    } else if (upload.status == UPLOAD_FILE_ABORTED) {
        abortSignedInstall("upload aborted");
        uploadAccepted = false;
        uploadError = "upload aborted";
    }
}

void completeUpload() {
    if (!uploadAccepted) {
        server->send(400, "application/json",
                     String("{\"ok\":false,\"error\":\"") + uploadError + "\"}");
        return;
    }
    if (!rebootToInstalledImage()) {
        server->send(500, "application/json", "{\"ok\":false,\"error\":\"OTA target partition unavailable\"}");
        return;
    }
    server->send(200, "application/json",
        String("{\"ok\":true,\"rebooting\":true,\"target_partition\":\"") +
        esp_ota_get_boot_partition()->label + "\"}");
    delay(250);
    ESP.restart();
}

void info() {
    char id[20];
    snprintf(id, sizeof(id), "%012llx", ESP.getEfuseMac());
    const uint32_t remaining = validationRemainingMs();
    server->send(200, "application/json", String("{\"board\":\"") + OTA_BOARD_ID +
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

bool verifySignedManifest(const String& manifest, const String& signatureBase64,
                          ManifestInfo& info, String& error) {
    error = "";
    info = {};
    String board, hash, version;
    uint32_t format = 0, imageSize = 0;
    if (manifest.isEmpty() || signatureBase64.isEmpty() ||
        !http_utils::jsonString(manifest, "board", board) ||
        !http_utils::jsonString(manifest, "sha256", hash) ||
        !http_utils::jsonString(manifest, "version", version) ||
        !http_utils::jsonUnsigned(manifest, "format", format) ||
        !http_utils::jsonUnsigned(manifest, "image_size", imageSize)) {
        error = "invalid manifest";
        return false;
    }

    String canonical;
    String channel, releaseTag, firmwareAsset;
    if (format == 1) {
        canonical = String("{\"board\":\"") + board + "\",\"format\":" + format +
            ",\"image_size\":" + imageSize + ",\"sha256\":\"" + hash +
            "\",\"version\":\"" + version + "\"}";
    } else if (format == 2 &&
               http_utils::jsonString(manifest, "channel", channel) &&
               http_utils::jsonString(manifest, "firmware_asset", firmwareAsset) &&
               http_utils::jsonString(manifest, "release_tag", releaseTag)) {
        canonical = String("{\"board\":\"") + board + "\",\"channel\":\"" + channel +
            "\",\"firmware_asset\":\"" + firmwareAsset + "\",\"format\":" + format +
            ",\"image_size\":" + imageSize + ",\"release_tag\":\"" + releaseTag +
            "\",\"sha256\":\"" + hash + "\",\"version\":\"" + version + "\"}";
        const String expectedAsset = String("firmware-") + board + ".bin";
        if (channel != "stable" || !semver::isStable(version.c_str()) ||
            releaseTag != String("v") + version || firmwareAsset != expectedAsset) {
            error = "invalid Internet release identity";
            return false;
        }
    } else {
        error = "unsupported manifest format";
        return false;
    }

    if (manifest != canonical) {
        error = "manifest is not canonical";
        return false;
    }
    if (board != OTA_BOARD_ID) {
        error = "firmware board does not match";
        return false;
    }
    if (imageSize == 0 || imageSize > ESP.getFreeSketchSpace()) {
        error = "invalid manifest image_size";
        return false;
    }
    if (!http_utils::validSha256(hash)) {
        error = "invalid manifest sha256";
        return false;
    }
    if (!verifyManifestSignature(manifest, signatureBase64, error)) return false;

    info.format = format;
    info.imageSize = imageSize;
    strlcpy(info.board, board.c_str(), sizeof(info.board));
    strlcpy(info.version, version.c_str(), sizeof(info.version));
    strlcpy(info.sha256, hash.c_str(), sizeof(info.sha256));
    strlcpy(info.channel, channel.c_str(), sizeof(info.channel));
    strlcpy(info.releaseTag, releaseTag.c_str(), sizeof(info.releaseTag));
    strlcpy(info.firmwareAsset, firmwareAsset.c_str(), sizeof(info.firmwareAsset));
    return true;
}

bool beginSignedInstall(const String& manifest, const String& signatureBase64,
                        ManifestInfo& info, String& error) {
    bool expected = false;
    if (!writerBusy.compare_exchange_strong(expected, true)) {
        error = "another OTA operation is already active";
        return false;
    }
    if (pendingVerify) {
        error = "running OTA image is still being validated";
        writerBusy.store(false);
        return false;
    }
    if (!verifySignedManifest(manifest, signatureBase64, info, error)) {
        writerBusy.store(false);
        return false;
    }
    bool validVersion = false;
    const int versionComparison =
        semver::compare(info.version, OTA_FIRMWARE_VERSION, validVersion);
    if (!validVersion || versionComparison <= 0) {
        error = !validVersion ? "OTA versions must use Semantic Versioning"
                              : "OTA version must be newer than the running version";
        writerBusy.store(false);
        return false;
    }
    activeManifest = info;
    expectedSha256 = info.sha256;
    expectedVersion = info.version;
    expectedImageSize = info.imageSize;
    receivedImageSize = 0;
    pauseLiveServiceForUpload();
    mbedtls_sha256_init(&shaContext);
    if (mbedtls_sha256_starts(&shaContext, 0) != 0) {
        error = "image hash setup failed";
        mbedtls_sha256_free(&shaContext);
        writerBusy.store(false);
        resumeLiveServiceAfterUploadFailure();
        return false;
    }
    shaStarted = true;
    if (!Update.begin(expectedImageSize, U_FLASH)) {
        error = String("not enough OTA space: ") + Update.errorString();
        mbedtls_sha256_free(&shaContext);
        shaStarted = false;
        writerBusy.store(false);
        resumeLiveServiceAfterUploadFailure();
        return false;
    }
    return true;
}

bool writeSignedInstall(const uint8_t* data, size_t size, String& error) {
    if (!writerBusy.load() || !shaStarted || !data ||
        size > expectedImageSize - receivedImageSize) {
        error = "invalid OTA image length";
        abortSignedInstall(error.c_str());
        return false;
    }
    if (mbedtls_sha256_update(&shaContext, data, size) != 0 ||
        Update.write(const_cast<uint8_t*>(data), size) != size) {
        error = String("flash write failed: ") + Update.errorString();
        abortSignedInstall(error.c_str());
        return false;
    }
    receivedImageSize += size;
    return true;
}

bool finishSignedInstall(String& error) {
    uint8_t digest[32];
    if (!writerBusy.load() || !shaStarted ||
        receivedImageSize != expectedImageSize ||
        mbedtls_sha256_finish(&shaContext, digest) != 0 ||
        !hexDigest(digest, sizeof(digest)).equalsIgnoreCase(expectedSha256)) {
        error = "image sha256 or size does not match manifest";
        abortSignedInstall(error.c_str());
        return false;
    }
    mbedtls_sha256_free(&shaContext);
    shaStarted = false;
    if (!Update.end(true)) {
        error = String("OTA finalisation failed: ") + Update.errorString();
        writerBusy.store(false);
        resumeLiveServiceAfterUploadFailure();
        return false;
    }
    return true;
}

void abortSignedInstall(const char* reason) {
    if (writerBusy.load()) Update.abort();
    if (shaStarted) {
        mbedtls_sha256_free(&shaContext);
        shaStarted = false;
    }
    writerBusy.store(false);
    if (reason && *reason) Serial.printf("OTA aborted: %s\n", reason);
    resumeLiveServiceAfterUploadFailure();
}

bool installInProgress() { return writerBusy.load(); }
uint32_t installBytesReceived() { return receivedImageSize; }
uint32_t installExpectedBytes() { return expectedImageSize; }
const char* installTargetVersion() { return expectedVersion.c_str(); }

bool rebootToInstalledImage() {
    if (!writerBusy.load() || shaStarted) return false;
    const esp_partition_t* target = esp_ota_get_boot_partition();
    if (!target || target == esp_ota_get_running_partition()) return false;
    persistPendingAttempt(target->label, expectedVersion);
    return true;
}

void registerRoutes(WebServer& value) {
    server = &value;
    server->on("/api/v1/info", HTTP_GET, info);
    server->on("/api/v1/update", HTTP_POST, completeUpload, handleUpload);
}

void begin() {
    if (running) return;
    runningPartition = esp_ota_get_running_partition();
    bootPartition = esp_ota_get_boot_partition();
    if (runningPartition && esp_ota_get_state_partition(runningPartition, &runningState) == ESP_OK) {
        pendingVerify = runningState == ESP_OTA_IMG_PENDING_VERIFY;
        imageConfirmed = !pendingVerify;
    }
    recordBootOutcome();
    loadCurrentUpdateDate();
    running = web_server::begin();
}

void update() {
    if (running) web_server::update();
    if (imageConfirmed) backfillCurrentUpdateDate();
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
const char* rollbackVersion() { return rollbackVersionValue.c_str(); }
bool rollbackSupported() { return esp_ota_check_rollback_is_possible(); }
int64_t lastUpdateUnixSeconds() { return lastUpdateUnixSecondsValue; }

uint32_t validationRemainingMs() {
    if (!pendingVerify || !applicationReady) return 0;
    const uint32_t elapsed = millis() - validationStartedAt;
    return elapsed >= kValidationWindowMs ? 0 : kValidationWindowMs - elapsed;
}

} // namespace ota_service
