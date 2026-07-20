#include "ota_service.h"
#include "ota_routes.h"

#include <Arduino.h>
#include <Preferences.h>
#include <Update.h>
#include <WebServer.h>
#include <esp_heap_caps.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_system.h>
#include <mbedtls/base64.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>

#include "ota_public_key.h"
#include "network/http_utils.h"
#include "network/live_websocket_service.h"
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
    if (mbedtls_sha256(reinterpret_cast<const unsigned char*>(manifest.c_str()),
                       manifest.length(), digest, 0) != 0) {
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
    if (!http_utils::authorised(*server)) { uploadError = "unauthorised"; return false; }
    const String manifest = server->arg("manifest");
    const String signature = server->arg("signature");
    String board, hash, version;
    uint32_t format = 0, imageSize = 0;
    if (manifest.isEmpty() || signature.isEmpty() ||
        !http_utils::jsonString(manifest, "board", board) ||
        !http_utils::jsonString(manifest, "sha256", hash) ||
        !http_utils::jsonString(manifest, "version", version) ||
        !http_utils::jsonUnsigned(manifest, "format", format) ||
        !http_utils::jsonUnsigned(manifest, "image_size", imageSize)) {
        uploadError = "invalid manifest";
        return false;
    }
    const String canonical = String("{\"board\":\"") + board + "\",\"format\":" + format +
        ",\"image_size\":" + imageSize + ",\"sha256\":\"" + hash +
        "\",\"version\":\"" + version + "\"}";
    if (format != 1 || manifest != canonical) { uploadError = "manifest is not canonical format 1"; return false; }
    if (board != OTA_BOARD_ID) { uploadError = "firmware board does not match"; return false; }
    if (imageSize == 0) { uploadError = "invalid manifest image_size"; return false; }
    if (!http_utils::validSha256(hash)) { uploadError = "invalid manifest sha256"; return false; }
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
        server->send(uploadError == "unauthorised" ? 401 : 400, "application/json",
                     String("{\"ok\":false,\"error\":\"") + uploadError + "\"}");
        return;
    }
    const esp_partition_t* target = esp_ota_get_next_update_partition(nullptr);
    if (!target) {
        resumeLiveServiceAfterUploadFailure();
        server->send(500, "application/json", "{\"ok\":false,\"error\":\"OTA target partition unavailable\"}");
        return;
    }
    persistPendingAttempt(target->label, expectedVersion);
    server->send(200, "application/json",
        String("{\"ok\":true,\"rebooting\":true,\"target_partition\":\"") + target->label + "\"}");
    delay(250);
    ESP.restart();
}

void info() {
    if (!http_utils::authorised(*server)) {
        server->send(401, "application/json", "{\"error\":\"unauthorised\"}");
        return;
    }
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
    running = web_server::begin();
}

void update() {
    if (running) web_server::update();
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
    return elapsed >= kValidationWindowMs ? 0 : kValidationWindowMs - elapsed;
}

} // namespace ota_service
