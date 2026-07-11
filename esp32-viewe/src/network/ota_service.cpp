#include "ota_service.h"

#include <Arduino.h>
#include <WebServer.h>
#include <Update.h>
#include <esp_system.h>
#include <mbedtls/base64.h>
#include <mbedtls/pk.h>
#include <mbedtls/rsa.h>
#include <mbedtls/sha256.h>

// These definitions are deliberately supplied by the local build environment,
// never committed credentials.  An empty value disables firmware updates.
#include "ota_public_key.h" // generated locally from keys/ota_signing_public.pem

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
bool running = false;
bool uploadAccepted = false;
String uploadError;
String expectedSha256;
uint32_t expectedImageSize = 0;
uint32_t receivedImageSize = 0;
mbedtls_sha256_context shaContext;
bool shaStarted = false;

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
    int end = start;
    while (end < static_cast<int>(json.length()) && isdigit(static_cast<unsigned char>(json[end]))) ++end;
    if (end == start || (end - start) > 10) return false;
    for (int i = start; i < end; ++i) value = value * 10 + (json[i] - '0');
    return true;
}

// The canonical manifest bytes themselves are signed with RSA-PSS/SHA-256.
bool verifyManifestSignature(const String& manifest, const String& signatureBase64) {
    if (strlen(OTA_SIGNING_PUBLIC_KEY_PEM) == 0) {
        uploadError = "signing public key is not configured";
        return false;
    }
    size_t signatureLength = 0;
    int rc = mbedtls_base64_decode(nullptr, 0, &signatureLength,
                                   reinterpret_cast<const unsigned char*>(signatureBase64.c_str()),
                                   signatureBase64.length());
    if (rc != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL || signatureLength == 0 || signatureLength > 512) {
        uploadError = "invalid base64 signature";
        return false;
    }
    uint8_t signature[512];
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
    rc = mbedtls_pk_parse_public_key(&key,
            reinterpret_cast<const unsigned char*>(OTA_SIGNING_PUBLIC_KEY_PEM),
            strlen(OTA_SIGNING_PUBLIC_KEY_PEM) + 1);
    if (rc == 0 && !mbedtls_pk_can_do(&key, MBEDTLS_PK_RSA)) rc = MBEDTLS_ERR_PK_TYPE_MISMATCH;
    if (rc == 0 && signatureLength != mbedtls_rsa_get_len(mbedtls_pk_rsa(key))) {
        rc = MBEDTLS_ERR_RSA_BAD_INPUT_DATA;
    }
    if (rc == 0) {
        rc = mbedtls_rsa_rsassa_pss_verify(mbedtls_pk_rsa(key), MBEDTLS_MD_SHA256,
                                           sizeof(digest), digest, signature);
    }
    mbedtls_pk_free(&key);
    if (rc != 0) {
        uploadError = "manifest signature verification failed";
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
    if (!verifyManifestSignature(manifest, signature)) return false;
    expectedSha256 = hash;
    expectedImageSize = imageSize;
    receivedImageSize = 0;
    mbedtls_sha256_init(&shaContext);
    if (mbedtls_sha256_starts(&shaContext, 0) != 0) { uploadError = "image hash setup failed"; return false; }
    shaStarted = true;
    if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) { uploadError = String("not enough OTA space: ") + Update.errorString(); return false; }
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
    }
}

void completeUpload() {
    if (!uploadAccepted) {
        server.send(uploadError == "unauthorised" ? 401 : 400, "application/json",
                    String("{\"ok\":false,\"error\":\"") + uploadError + "\"}");
        return;
    }
    server.send(200, "application/json", "{\"ok\":true,\"rebooting\":true}");
    delay(250); // flush HTTP response before booting the selected OTA partition
    ESP.restart();
}

void info() {
    if (!authorised()) { server.send(401, "application/json", "{\"error\":\"unauthorised\"}"); return; }
    char id[20];
    snprintf(id, sizeof(id), "%012llx", ESP.getEfuseMac());
    server.send(200, "application/json", String("{\"board\":\"") + OTA_BOARD_ID +
                "\",\"version\":\"" + OTA_FIRMWARE_VERSION + "\",\"hardware_id\":\"" + id + "\"}");
}

} // namespace

void begin() {
    if (running) return;
    const char* headers[] = {"X-OTA-Token"};
    server.collectHeaders(headers, 1);
    server.on("/api/v1/info", HTTP_GET, info);
    server.on("/api/v1/update", HTTP_POST, completeUpload, handleUpload);
    server.begin();
    running = true;
}

void update() { if (running) server.handleClient(); }
bool isRunning() { return running; }

} // namespace ota_service
