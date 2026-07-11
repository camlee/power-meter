#include "ota_service.h"

#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>
#include <WebServer.h>
#include <Update.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_system.h>
#include <mbedtls/base64.h>
#include <mbedtls/pk.h>
#include <mbedtls/rsa.h>
#include <mbedtls/sha256.h>

// These definitions are deliberately supplied by the local build environment,
// never committed credentials.  An empty value disables firmware updates.
#include "ota_public_key.h" // generated locally from keys/ota_signing_public.pem
#include "lvgl_v8_port.h"
#include "ui/remote_input.h"

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
String expectedVersion;
uint32_t expectedImageSize = 0;
uint32_t receivedImageSize = 0;
mbedtls_sha256_context shaContext;
bool shaStarted = false;
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

bool jsonBool(const String& json, const char* name, bool& value) {
    String key = String("\"") + name + "\"";
    int keyAt = json.indexOf(key);
    if (keyAt < 0) return false;
    int colon = json.indexOf(':', keyAt + key.length());
    if (colon < 0) return false;
    const String tail = json.substring(colon + 1);
    if (tail.startsWith("true")) { value = true; return true; }
    if (tail.startsWith("false")) { value = false; return true; }
    return false;
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

// The RGB panel retains the rendered image in its framebuffer, making this a
// faithful capture of the physical display rather than a second UI renderer.
void screenshot() {
    if (!authorised()) { server.send(401, "application/json", "{\"error\":\"unauthorised\"}"); return; }
    uint16_t width = 0, height = 0;
    const uint8_t* frameBuffer = lvgl_port_get_remote_framebuffer(&width, &height);
    if (!frameBuffer) {
        server.send(503, "application/json", "{\"error\":\"framebuffer unavailable\"}");
        return;
    }

    // PPM is useful for CLI automation and is directly readable by common
    // image tooling without an encoder. The browser viewer uses BMP below.
    if (server.arg("format") == "ppm") {
        const String ppmHeader = String("P6\n") + width + " " + height + "\n255\n";
        server.setContentLength(ppmHeader.length() + static_cast<size_t>(width) * height * 3);
        server.send(200, "image/x-portable-pixmap", "");
        WiFiClient client = server.client();
        client.write(reinterpret_cast<const uint8_t*>(ppmHeader.c_str()), ppmHeader.length());
        uint8_t line[320 * 3];
        for (uint16_t y = 0; y < height && client.connected(); ++y) {
            for (uint16_t x = 0; x < width; ++x) {
                const size_t source = (static_cast<size_t>(y) * width + x) * 2;
#if LV_COLOR_16_SWAP
                const uint16_t rgb565 = (static_cast<uint16_t>(frameBuffer[source]) << 8) | frameBuffer[source + 1];
#else
                const uint16_t rgb565 = frameBuffer[source] | (static_cast<uint16_t>(frameBuffer[source + 1]) << 8);
#endif
                line[x * 3] = ((rgb565 >> 11) & 0x1f) * 255 / 31;
                line[x * 3 + 1] = ((rgb565 >> 5) & 0x3f) * 255 / 63;
                line[x * 3 + 2] = (rgb565 & 0x1f) * 255 / 31;
            }
            client.write(line, width * 3);
        }
        return;
    }

    // 24-bit BMP is universally displayable in browsers and requires no JPEG
    // encoder or a second full-size output buffer on the ESP32.
    constexpr size_t kHeaderBytes = 54;
    const uint32_t pixelBytes = static_cast<uint32_t>(width) * height * 3;
    uint8_t header[kHeaderBytes] = {};
    header[0] = 'B'; header[1] = 'M';
    putLe32(header + 2, kHeaderBytes + pixelBytes);
    putLe32(header + 10, kHeaderBytes);
    putLe32(header + 14, 40);
    putLe32(header + 18, width);
    putLe32(header + 22, height);
    putLe16(header + 26, 1);
    putLe16(header + 28, 24);
    putLe32(header + 34, pixelBytes);

    server.setContentLength(kHeaderBytes + pixelBytes);
    server.send(200, "image/bmp", "");
    WiFiClient client = server.client();
    client.write(header, sizeof(header));
    uint8_t line[320 * 3]; // Current panel width; send in chunks for wider boards.
    for (int y = height - 1; y >= 0 && client.connected(); --y) {
        size_t x = 0;
        while (x < width) {
            const size_t pixels = min<size_t>(width - x, sizeof(line) / 3);
            for (size_t i = 0; i < pixels; ++i) {
                const size_t source = (static_cast<size_t>(y) * width + x + i) * 2;
#if LV_COLOR_16_SWAP
                const uint16_t rgb565 = (static_cast<uint16_t>(frameBuffer[source]) << 8) | frameBuffer[source + 1];
#else
                const uint16_t rgb565 = frameBuffer[source] | (static_cast<uint16_t>(frameBuffer[source + 1]) << 8);
#endif
                const uint8_t r = ((rgb565 >> 11) & 0x1f) * 255 / 31;
                const uint8_t g = ((rgb565 >> 5) & 0x3f) * 255 / 63;
                const uint8_t b = (rgb565 & 0x1f) * 255 / 31;
                line[i * 3] = b;
                line[i * 3 + 1] = g;
                line[i * 3 + 2] = r;
            }
            client.write(line, pixels * 3);
            x += pixels;
        }
        delay(0);
    }
}

bool parseRemotePoint(uint16_t& x, uint16_t& y, bool* pressed = nullptr) {
    if (!authorised()) { server.send(401, "application/json", "{\"error\":\"unauthorised\"}"); return false; }
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

const char kRemoteViewer[] PROGMEM = R"HTML(<!doctype html><meta name=viewport content="width=device-width,initial-scale=1"><title>power-meter remote</title><style>body{font:16px system-ui;margin:1rem;background:#111;color:#eee}input,button{font:inherit;padding:.45rem}img{display:block;width:min(100%,480px);height:auto;margin-top:1rem;touch-action:none;background:#222}</style><h1>power-meter remote</h1><p>Enter the device API token. It is saved only in this browser for this device. The display refreshes about once per second.</p><input id=t type=password placeholder="API token" autocomplete=off><button id=c>Connect</button><button id=f>Forget token</button><span id=s></span><p id=e>Connect to view and control the display.</p><img id=i alt="Device display" hidden><script>const t=document.querySelector('#t'),i=document.querySelector('#i'),s=document.querySelector('#s'),e=document.querySelector('#e'),key='power-meter.remote.token';let active=false,last='',lastMove=0;t.value=localStorage.getItem(key)||'';const h=()=>({Authorization:'Bearer '+t.value}),pos=x=>{let r=i.getBoundingClientRect();return{x:Math.max(0,Math.min(319,Math.round((x.clientX-r.left)*320/r.width))),y:Math.max(0,Math.min(479,Math.round((x.clientY-r.top)*480/r.height)))}};async function shot(){if(!active)return;try{let r=await fetch('/api/v1/display/screenshot.bmp',{headers:h()});if(!r.ok)throw Error(r.status);let u=URL.createObjectURL(await r.blob());URL.revokeObjectURL(last);last=u;i.src=u;i.hidden=false;e.hidden=true;s.textContent=' connected'}catch(x){s.textContent=' '+x}setTimeout(shot,900)}async function pointer(x,pressed){if(!active)return;await fetch('/api/v1/display/pointer',{method:'POST',headers:{...h(),'Content-Type':'application/json'},body:JSON.stringify({...pos(x),pressed})})}document.querySelector('#c').onclick=()=>{if(!t.value)return;s.textContent=' connecting';localStorage.setItem(key,t.value);active=true;shot()};document.querySelector('#f').onclick=()=>{localStorage.removeItem(key);t.value='';active=false;i.hidden=true;e.hidden=false;s.textContent=' token forgotten'};i.addEventListener('pointerdown',x=>{i.setPointerCapture(x.pointerId);pointer(x,true)});i.addEventListener('pointermove',x=>{if(x.buttons&&Date.now()-lastMove>60){lastMove=Date.now();pointer(x,true)}});i.addEventListener('pointerup',x=>pointer(x,false));i.addEventListener('pointercancel',x=>pointer(x,false));</script>)HTML";

void remoteViewer() {
    server.send_P(200, "text/html; charset=utf-8", kRemoteViewer);
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
    expectedVersion = version;
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
    const esp_partition_t* target = esp_ota_get_next_update_partition(nullptr);
    if (!target) {
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
    const char* headers[] = {"X-OTA-Token"};
    server.collectHeaders(headers, 1);
    server.on("/api/v1/info", HTTP_GET, info);
    server.on("/api/v1/update", HTTP_POST, completeUpload, handleUpload);
    server.on("/api/v1/display/screenshot.bmp", HTTP_GET, screenshot);
    server.on("/api/v1/display/tap", HTTP_POST, remoteTap);
    server.on("/api/v1/display/pointer", HTTP_POST, remotePointer);
    server.on("/remote", HTTP_GET, remoteViewer);
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
