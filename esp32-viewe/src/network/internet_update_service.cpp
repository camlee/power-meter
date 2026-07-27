#include "internet_update_service.h"

#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_system.h>
#include <mbedtls/base64.h>
#include <atomic>
#include <time.h>

#include "device/device_state.h"
#include "network/http_utils.h"
#include "network/network_manager.h"
#include "network/ota_service.h"
#include "network/semver.h"
#include "network/update_policy.h"
#include "ota_public_key.h"

#ifndef OTA_FIRMWARE_VERSION
#define OTA_FIRMWARE_VERSION "0.0.0-dev"
#endif
#ifndef OTA_BOARD_ID
#define OTA_BOARD_ID "meter"
#endif
#ifndef OTA_RELEASE_REPOSITORY
#define OTA_RELEASE_REPOSITORY "camlee/power-meter"
#endif

namespace internet_update_service {
namespace {

constexpr size_t kMaxDescriptorBytes = 2048;
constexpr size_t kMaxManifestBytes = 768;
constexpr uint32_t kInitialJitterMinMs = 60U * 1000U;
constexpr uint32_t kInitialJitterRangeMs = 4U * 60U * 1000U;
constexpr int64_t kEarliestTlsTime = 1577836800LL; // 2020-01-01

enum class Operation : uint8_t {
    None,
    ManualCheck,
    AutomaticCheck,
    Install,
};

struct HttpContext {
    bool firmware = false;
    bool failed = false;
    String body;
    String error;
};

Status statusValue{};
SemaphoreHandle_t statusMutex = nullptr;
std::atomic<bool> workerActive{false};
std::atomic<uint8_t> requestedOperation{static_cast<uint8_t>(Operation::None)};
String availableManifest;
String availableSignature;
ota_service::ManifestInfo availableInfo{};
uint8_t consecutiveFailures = 0;
uint32_t retryNotBeforeMs = 0;
uint32_t automaticNotBeforeMs = 0;
bool previousInternet = false;
bool initialized = false;
bool rollbackReconciled = false;
bool confirmationReconciled = false;
String highestConfirmedVersion;

bool timeReached(uint32_t deadline, uint32_t now = millis()) {
    return deadline != 0 && static_cast<int32_t>(now - deadline) >= 0;
}

bool validWallTime() {
    return static_cast<int64_t>(time(nullptr)) >= kEarliestTlsTime;
}

void mutateStatus(void (*callback)(Status&)) {
    if (!statusMutex || xSemaphoreTake(statusMutex, pdMS_TO_TICKS(100)) != pdTRUE) return;
    callback(statusValue);
    xSemaphoreGive(statusMutex);
    device_state::changed(device_state::Domain::Update);
}

void setState(State state, const char* error = nullptr) {
    if (!statusMutex || xSemaphoreTake(statusMutex, pdMS_TO_TICKS(100)) != pdTRUE) return;
    const bool changed = statusValue.state != state ||
        statusValue.busy != (state == State::Checking || state == State::Downloading ||
                             state == State::Verifying || state == State::Rebooting) ||
        (error && strcmp(statusValue.error, error) != 0) ||
        (!error && statusValue.error[0] != '\0');
    statusValue.state = state;
    statusValue.busy = state == State::Checking || state == State::Downloading ||
                       state == State::Verifying || state == State::Rebooting;
    if (error) strlcpy(statusValue.error, error, sizeof(statusValue.error));
    else statusValue.error[0] = '\0';
    if (state != State::Downloading) statusValue.progressPercent = 0;
    xSemaphoreGive(statusMutex);
    if (changed) device_state::changed(device_state::Domain::Update);
}

void setAvailable(const ota_service::ManifestInfo& info, State state = State::Available) {
    if (!statusMutex || xSemaphoreTake(statusMutex, pdMS_TO_TICKS(100)) != pdTRUE) return;
    strlcpy(statusValue.availableVersion, info.version,
            sizeof(statusValue.availableVersion));
    statusValue.state = state;
    statusValue.busy = false;
    statusValue.error[0] = '\0';
    statusValue.progressPercent = 0;
    xSemaphoreGive(statusMutex);
    device_state::changed(device_state::Domain::Update);
}

void clearAvailable() {
    availableManifest = "";
    availableSignature = "";
    availableInfo = {};
    mutateStatus([](Status& status) { status.availableVersion[0] = '\0'; });
}

void persistLastCheck(int64_t now) {
    Preferences prefs;
    if (prefs.begin("internet_ota", false)) {
        prefs.putLong64("last_check", now);
        prefs.end();
    }
    if (!statusMutex || xSemaphoreTake(statusMutex, pdMS_TO_TICKS(100)) != pdTRUE) return;
    statusValue.lastCheckUnixSeconds = now;
    statusValue.nextCheckUnixSeconds = now + update_policy::kCheckIntervalSeconds;
    xSemaphoreGive(statusMutex);
}

esp_err_t httpEvent(esp_http_client_event_t* event) {
    auto* context = static_cast<HttpContext*>(event->user_data);
    if (!context || event->event_id != HTTP_EVENT_ON_DATA || event->data_len <= 0) {
        return ESP_OK;
    }
    // Auto-followed GitHub redirects may have their own small response body.
    // Only the final successful response belongs to the descriptor/image.
    if (esp_http_client_get_status_code(event->client) != 200) return ESP_OK;
    if (!context->firmware) {
        if (context->body.length() + static_cast<size_t>(event->data_len) >
            kMaxDescriptorBytes) {
            context->failed = true;
            context->error = "update descriptor is too large";
            return ESP_FAIL;
        }
        context->body.concat(static_cast<const char*>(event->data),
                             static_cast<unsigned>(event->data_len));
        return ESP_OK;
    }

    String error;
    if (!ota_service::writeSignedInstall(
            static_cast<const uint8_t*>(event->data),
            static_cast<size_t>(event->data_len), error)) {
        context->failed = true;
        context->error = error;
        return ESP_FAIL;
    }
    const uint32_t expected = ota_service::installExpectedBytes();
    const uint64_t calculated = expected
        ? (ota_service::installBytesReceived() * 100ULL) / expected : 0;
    const uint8_t progress =
        static_cast<uint8_t>(calculated > 99 ? 99 : calculated);
    if (statusMutex && xSemaphoreTake(statusMutex, 0) == pdTRUE) {
        statusValue.progressPercent = progress;
        xSemaphoreGive(statusMutex);
    }
    return ESP_OK;
}

bool performGet(const char* url, HttpContext& context, String& error) {
    esp_http_client_config_t config{};
    config.url = url;
    config.method = HTTP_METHOD_GET;
    config.timeout_ms = 10000;
    config.disable_auto_redirect = false;
    config.max_redirection_count = 5;
    config.event_handler = httpEvent;
    config.user_data = &context;
    config.user_agent = "power-meter-ota/1";
    config.buffer_size = 4096;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.skip_cert_common_name_check = false;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        error = "could not allocate HTTPS client";
        return false;
    }
    esp_http_client_set_header(client, "Accept",
                               context.firmware ? "application/octet-stream" :
                                                  "application/json");
    const esp_err_t result = esp_http_client_perform(client);
    const int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (result != ESP_OK) {
        error = context.error.length()
            ? context.error
            : String("HTTPS request failed: ") + esp_err_to_name(result);
        return false;
    }
    if (context.failed) {
        error = context.error;
        return false;
    }
    if (status != 200) {
        error = String("GitHub returned HTTP ") + status;
        return false;
    }
    return true;
}

bool decodeBase64(const String& encoded, String& decoded, size_t maximum,
                  String& error) {
    size_t required = 0;
    const int probe = mbedtls_base64_decode(
        nullptr, 0, &required,
        reinterpret_cast<const unsigned char*>(encoded.c_str()), encoded.length());
    if (probe != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL || required == 0 ||
        required > maximum) {
        error = "invalid descriptor base64";
        return false;
    }
    auto* buffer = static_cast<unsigned char*>(malloc(required + 1));
    if (!buffer) {
        error = "not enough memory for update descriptor";
        return false;
    }
    size_t written = 0;
    const int result = mbedtls_base64_decode(
        buffer, required, &written,
        reinterpret_cast<const unsigned char*>(encoded.c_str()), encoded.length());
    if (result == 0) {
        buffer[written] = '\0';
        decoded = String(reinterpret_cast<const char*>(buffer), written);
    }
    free(buffer);
    if (result != 0) {
        error = "invalid descriptor base64";
        return false;
    }
    return true;
}

bool fetchDescriptor(String& manifest, String& signature,
                     ota_service::ManifestInfo& info, String& error) {
    char url[256];
    snprintf(url, sizeof(url),
             "https://github.com/%s/releases/latest/download/ota-%s.json",
             OTA_RELEASE_REPOSITORY, OTA_BOARD_ID);
    HttpContext context{};
    context.body.reserve(1024);
    if (!performGet(url, context, error)) return false;

    String manifestBase64, signatureBase64;
    if (!http_utils::jsonString(context.body, "manifest_b64", manifestBase64) ||
        !http_utils::jsonString(context.body, "signature_b64", signatureBase64) ||
        !decodeBase64(manifestBase64, manifest, kMaxManifestBytes, error)) {
        if (error.isEmpty()) error = "invalid update descriptor";
        return false;
    }
    signature = signatureBase64;
    return ota_service::verifySignedManifest(manifest, signature, info, error);
}

bool versionIsNewer(const char* candidate, String& error) {
    bool valid = false;
    if (semver::compare(candidate, OTA_FIRMWARE_VERSION, valid) <= 0 || !valid) {
        if (!valid) error = "running firmware version is not valid SemVer";
        return false;
    }
    if (!highestConfirmedVersion.isEmpty()) {
        const int highComparison =
            semver::compare(candidate, highestConfirmedVersion.c_str(), valid);
        if (!valid) {
            error = "stored confirmed firmware version is invalid";
            return false;
        }
        if (highComparison <= 0) return false;
    }
    return true;
}

bool installAvailable(String& error) {
    ota_service::ManifestInfo info{};
    if (!ota_service::beginSignedInstall(availableManifest, availableSignature,
                                         info, error)) return false;
    setState(State::Downloading);
    char url[320];
    snprintf(url, sizeof(url),
             "https://github.com/%s/releases/download/%s/%s",
             OTA_RELEASE_REPOSITORY, info.releaseTag, info.firmwareAsset);
    HttpContext context{};
    context.firmware = true;
    if (!performGet(url, context, error)) {
        ota_service::abortSignedInstall(error.c_str());
        return false;
    }
    setState(State::Verifying);
    if (!ota_service::finishSignedInstall(error)) return false;
    setState(State::Rebooting);
    if (!ota_service::rebootToInstalledImage()) {
        error = "OTA boot partition was not selected";
        ota_service::abortSignedInstall(error.c_str());
        return false;
    }
    delay(500);
    ESP.restart();
    return true;
}

void recordFailure(const String& error) {
    ++consecutiveFailures;
    retryNotBeforeMs = millis() +
        update_policy::retryDelayMs(consecutiveFailures) +
        (esp_random() % 30000U);
    setState(State::Failed, error.c_str());
}

void performCheck(bool automatic) {
    setState(State::Checking);
    String manifest, signature, error;
    ota_service::ManifestInfo info{};
    if (!fetchDescriptor(manifest, signature, info, error)) {
        recordFailure(error);
        return;
    }
    const int64_t now = static_cast<int64_t>(time(nullptr));
    persistLastCheck(now);
    consecutiveFailures = 0;
    retryNotBeforeMs = 0;

    String comparisonError;
    if (!versionIsNewer(info.version, comparisonError)) {
        clearAvailable();
        if (!comparisonError.isEmpty()) recordFailure(comparisonError);
        else setState(State::UpToDate);
        return;
    }

    Status snapshot{};
    getStatus(snapshot);
    if (snapshot.blockedVersion[0] != '\0' &&
        strcmp(snapshot.blockedVersion, info.version) == 0) {
        availableManifest = manifest;
        availableSignature = signature;
        availableInfo = info;
        setAvailable(info, State::BlockedAfterRollback);
        return;
    }

    availableManifest = manifest;
    availableSignature = signature;
    availableInfo = info;
    setAvailable(info);
    if (automatic && snapshot.automatic) {
        if (!installAvailable(error)) recordFailure(error);
    }
}

void workerTask(void* argument) {
    const Operation operation =
        static_cast<Operation>(reinterpret_cast<uintptr_t>(argument));
    if (operation == Operation::Install) {
        String error;
        if (!installAvailable(error)) recordFailure(error);
    } else {
        performCheck(operation == Operation::AutomaticCheck);
    }
    workerActive.store(false);
    vTaskDelete(nullptr);
}

bool startWorker(Operation operation) {
    bool expected = false;
    if (!workerActive.compare_exchange_strong(expected, true)) return false;
    if (xTaskCreate(workerTask, "internet_ota", 12288,
                    reinterpret_cast<void*>(static_cast<uintptr_t>(operation)),
                    1, nullptr) != pdPASS) {
        workerActive.store(false);
        setState(State::Failed, "could not start update worker");
        return false;
    }
    return true;
}

void loadPreferences() {
    Preferences prefs;
    if (!prefs.begin("internet_ota", true)) return;
    statusValue.automatic = prefs.getBool("automatic", true);
    statusValue.lastCheckUnixSeconds = prefs.getLong64("last_check", 0);
    if (statusValue.lastCheckUnixSeconds > 0) {
        statusValue.nextCheckUnixSeconds =
            statusValue.lastCheckUnixSeconds +
            update_policy::kCheckIntervalSeconds;
    }
    highestConfirmedVersion = prefs.getString("highest", "");
    const String blocked = prefs.getString("blocked", "");
    strlcpy(statusValue.blockedVersion, blocked.c_str(),
            sizeof(statusValue.blockedVersion));
    prefs.end();
}

void persistVersionState(const char* key, const char* value) {
    Preferences prefs;
    if (!prefs.begin("internet_ota", false)) return;
    prefs.putString(key, value ? value : "");
    prefs.end();
}

void reconcileBootOutcome() {
    if (!rollbackReconciled && ota_service::rollbackDetected() &&
        ota_service::rollbackVersion()[0]) {
        const char* rolledBack = ota_service::rollbackVersion();
        strlcpy(statusValue.blockedVersion, rolledBack,
                sizeof(statusValue.blockedVersion));
        persistVersionState("blocked", rolledBack);
        rollbackReconciled = true;
    }
    if (confirmationReconciled) return;
    if (strcmp(ota_service::healthStatus(), "confirmed") != 0 ||
        !semver::isStable(OTA_FIRMWARE_VERSION)) return;
    bool valid = false;
    if (highestConfirmedVersion.isEmpty() ||
        semver::compare(OTA_FIRMWARE_VERSION,
                        highestConfirmedVersion.c_str(), valid) > 0) {
        highestConfirmedVersion = OTA_FIRMWARE_VERSION;
        persistVersionState("highest", highestConfirmedVersion.c_str());
    }
    confirmationReconciled = true;
}

} // namespace

void begin() {
    if (initialized) return;
    statusMutex = xSemaphoreCreateMutex();
    strlcpy(statusValue.currentVersion, OTA_FIRMWARE_VERSION,
            sizeof(statusValue.currentVersion));
    loadPreferences();
    reconcileBootOutcome();
    statusValue.state = State::WaitingForNetwork;
    initialized = true;
}

void update() {
    if (!initialized) return;
    reconcileBootOutcome();

    const bool internet =
        network_manager::getState() ==
        network_manager::NetworkState::ConnectedStaInternet;
    const bool clockReady = validWallTime();
    const uint32_t nowMs = millis();
    const int64_t nowUnix = static_cast<int64_t>(time(nullptr));

    const Operation requested = static_cast<Operation>(
        requestedOperation.exchange(static_cast<uint8_t>(Operation::None)));
    if (requested != Operation::None) {
        if (!internet) {
            setState(State::WaitingForNetwork, "Internet connection is unavailable");
        } else if (!clockReady) {
            setState(State::WaitingForTime, "Waiting for network time before HTTPS");
        } else if (requested == Operation::Install &&
                   !availableInfo.version[0]) {
            setState(State::Failed, "No installable update is available");
        } else {
            startWorker(requested);
        }
    }

    if (workerActive.load()) {
        previousInternet = internet;
        return;
    }
    if (!internet) {
        previousInternet = false;
        setState(State::WaitingForNetwork);
        return;
    }
    if (!clockReady) {
        previousInternet = true;
        setState(State::WaitingForTime);
        return;
    }

    Status snapshot{};
    getStatus(snapshot);
    const bool due = update_policy::automaticCheckDue(
        snapshot.lastCheckUnixSeconds != 0,
        snapshot.lastCheckUnixSeconds, nowUnix);
    if (due && automaticNotBeforeMs == 0 && retryNotBeforeMs == 0) {
        automaticNotBeforeMs = nowMs + kInitialJitterMinMs +
                               (esp_random() % kInitialJitterRangeMs);
    }
    previousInternet = true;

    if (retryNotBeforeMs && timeReached(retryNotBeforeMs, nowMs)) {
        retryNotBeforeMs = 0;
        startWorker(Operation::AutomaticCheck);
    } else if (due && automaticNotBeforeMs &&
               timeReached(automaticNotBeforeMs, nowMs)) {
        automaticNotBeforeMs = 0;
        startWorker(Operation::AutomaticCheck);
    } else if (snapshot.state == State::WaitingForNetwork ||
               snapshot.state == State::WaitingForTime) {
        setState(State::Idle);
    }
}

bool getStatus(Status& out) {
    if (!statusMutex ||
        xSemaphoreTake(statusMutex, pdMS_TO_TICKS(100)) != pdTRUE) return false;
    out = statusValue;
    out.updateDateUnixSeconds = ota_service::lastUpdateUnixSeconds();
    xSemaphoreGive(statusMutex);
    return true;
}

const char* stateName(State state) {
    switch (state) {
        case State::Idle: return "idle";
        case State::WaitingForNetwork: return "waiting_for_network";
        case State::WaitingForTime: return "waiting_for_time";
        case State::Checking: return "checking";
        case State::UpToDate: return "up_to_date";
        case State::Available: return "available";
        case State::Downloading: return "downloading";
        case State::Verifying: return "verifying";
        case State::Rebooting: return "rebooting";
        case State::Failed: return "failed";
        case State::BlockedAfterRollback: return "blocked_after_rollback";
    }
    return "idle";
}

bool requestCheck() {
    if (!initialized || workerActive.load() ||
        strcmp(ota_service::runningImageState(), "pending_verify") == 0) return false;
    requestedOperation.store(static_cast<uint8_t>(Operation::ManualCheck));
    return true;
}

bool requestInstall() {
    if (!initialized || workerActive.load() ||
        strcmp(ota_service::runningImageState(), "pending_verify") == 0) return false;
    Status snapshot{};
    if (!getStatus(snapshot) || snapshot.state != State::Available) return false;
    requestedOperation.store(static_cast<uint8_t>(Operation::Install));
    return true;
}

bool setAutomatic(bool enabled) {
    if (!initialized) return false;
    Preferences prefs;
    if (!prefs.begin("internet_ota", false)) return false;
    const bool saved = prefs.putBool("automatic", enabled) == sizeof(uint8_t);
    prefs.end();
    if (!saved) return false;
    if (!statusMutex ||
        xSemaphoreTake(statusMutex, pdMS_TO_TICKS(100)) != pdTRUE) return false;
    statusValue.automatic = enabled;
    xSemaphoreGive(statusMutex);
    device_state::changed(device_state::Domain::Update);
    return true;
}

} // namespace internet_update_service
