#include "network_manager.h"
#include "network_reconnect_policy.h"
#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_log.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include <NetworkClient.h>
#include <esp_sntp.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <time.h>
#include "local_config.h"
#include "device/device_identity.h"
#include "device/device_state.h"
#include "time/time_service.h"

namespace network_manager {
namespace {

NetworkState currentState = NetworkState::Disconnected;
ConnectionPhase connectionPhase = ConnectionPhase::Idle;
ConnectionFailure connectionFailure = ConnectionFailure::None;
uint32_t connectStartTime = 0;
char targetSsid[33] = {0};
char lastPassword[64] = {0};
bool apRunning = false; // Add this discrete AP state flag
bool mdnsRunning = false;
uint32_t nextReconnectAt = 0;
uint32_t reconnectDelayMs = 5000;
bool retryEnabled = false;
bool connectionStartPending = false;
uint32_t connectionStartNotBefore = 0;

enum class AttemptOrigin {
    None,
    Manual,
    FastRetry,
    RecoveryCandidate,
};

enum class PendingRecoveryAction {
    None,
    FastRetry,
    TryNextCandidate,
    Discover,
};

enum ScanPurpose : uint8_t {
    ScanPurposeNone = 0,
    ScanPurposeUser = 1 << 0,
    ScanPurposeRecovery = 1 << 1,
};

struct RecoveryCandidate {
    RecoveryCandidatePolicy policy{};
    char ssid[33]{};
    char password[64]{};
};

RecoveryState recoveryState = RecoveryState::Disabled;
AttemptOrigin attemptOrigin = AttemptOrigin::None;
PendingRecoveryAction pendingRecoveryAction = PendingRecoveryAction::None;
RecoveryCandidate recoveryCandidates[8];
size_t recoveryCandidateCount = 0;
char suppressedSsids[8][33]{};
size_t suppressedSsidCount = 0;

enum class ExpectedDisconnect {
    None,
    Reconfigure,
    Scan,
    FailureCleanup,
    Manual,
};

ExpectedDisconnect expectedDisconnect = ExpectedDisconnect::None;

// Scan state is independent of station connection state so a connected
// station can remain online while the radio surveys nearby networks.
int scanCount = 0;
ScanState scanState = ScanState::Idle;
uint32_t scanGeneration = 0;
uint32_t scanStartDeadline = 0;
uint32_t nextScanStartAttempt = 0;
uint8_t scanPurpose = ScanPurposeNone;

// A concise, machine-readable serial line is the reliable recovery path when
// mDNS is unavailable. Only emit it when an address or state changes: this is
// useful in a monitor log without turning normal operation into log spam.
uint32_t reportedStaIp = UINT32_MAX;
uint32_t reportedApIp = UINT32_MAX;
NetworkState reportedState = static_cast<NetworkState>(0xff);
ConnectionPhase reportedPhase = static_cast<ConnectionPhase>(0xff);
ConnectionFailure reportedFailure = static_cast<ConnectionFailure>(0xff);
ScanState reportedScanState = static_cast<ScanState>(0xff);
RecoveryState reportedRecoveryState = static_cast<RecoveryState>(0xff);

void reportWebAddressesIfChanged() {
    const IPAddress staIp = WiFi.localIP();
    const IPAddress apIp = WiFi.softAPIP();
    const uint32_t staRaw = static_cast<uint32_t>(staIp);
    const uint32_t apRaw = static_cast<uint32_t>(apIp);
    if (reportedStaIp == staRaw && reportedApIp == apRaw &&
        reportedState == currentState && reportedPhase == connectionPhase &&
        reportedFailure == connectionFailure && reportedScanState == scanState &&
        reportedRecoveryState == recoveryState) return;
    reportedStaIp = staRaw;
    reportedApIp = apRaw;
    reportedState = currentState;
    reportedPhase = connectionPhase;
    reportedFailure = connectionFailure;
    reportedScanState = scanState;
    reportedRecoveryState = recoveryState;

    const String staText = staIp.toString();
    const String apText = apIp.toString();
    const String apSsid = apRunning ? WiFi.softAPSSID() : String();
    const char* host = device_identity::getHostname();
    // ESP_LOG is visible through this board's USB JTAG serial device; Serial
    // keeps the same information available on the conventional framework UART.
    ESP_LOGI("network", "VIEWE_NETWORK state=%u phase=%u failure=%u scan=%u recovery=%u station=%s ap=%s host=%s.local",
             static_cast<unsigned>(currentState), static_cast<unsigned>(connectionPhase),
             static_cast<unsigned>(connectionFailure), static_cast<unsigned>(scanState),
             static_cast<unsigned>(recoveryState),
             staText.c_str(), apText.c_str(), host);
    Serial.printf("VIEWE_NETWORK state=%u phase=%u failure=%u scan=%u recovery=%u station=%s ap=%s ap_ssid=%s host=%s.local\n",
                  static_cast<unsigned>(currentState), static_cast<unsigned>(connectionPhase),
                  static_cast<unsigned>(connectionFailure), static_cast<unsigned>(scanState),
                  static_cast<unsigned>(recoveryState),
                  staText.c_str(), apText.c_str(), apSsid.c_str(), host);
    if (staRaw) {
        ESP_LOGI("network", "VIEWE_WEB url=http://%s/ host=%s.local", staText.c_str(), host);
        Serial.printf("VIEWE_WEB url=http://%s/ host=%s.local\n", staText.c_str(), host);
    }
    if (apRunning && apRaw) {
        ESP_LOGI("network", "VIEWE_WEB_AP url=http://%s/", apText.c_str());
        Serial.printf("VIEWE_WEB_AP url=http://%s/\n", apText.c_str());
    }
}

struct WifiEventRecord {
    arduino_event_id_t id;
    uint8_t disconnectReason;
};

StaticQueue_t wifiEventQueueStorage;
uint8_t wifiEventQueueBuffer[8 * sizeof(WifiEventRecord)];
QueueHandle_t wifiEventQueue = nullptr;

// SNTP configuration
constexpr char kNtpServer1[] = "pool.ntp.org";
constexpr char kNtpServer2[] = "time.nist.gov";
bool ntpConfigured = false;
volatile bool ntpSyncObserved = false;

// Internet probing runs off the UI/main task because DNS and a failed TCP
// connect may take over a second even with a bounded socket timeout.
constexpr char kConnectivityHost[] = "connectivitycheck.gstatic.com";
constexpr uint32_t kInternetProbeIntervalMs = 30000;
constexpr uint32_t kInternetProbeTimeoutMs = 1500;
volatile int8_t internetProbeResult = -1; // -1=pending/none, 0=failed, 1=HTTP 204
bool internetProbeInFlight = false;
uint32_t lastInternetProbeMs = 0;
uint8_t consecutiveInternetProbeFailures = 0;

constexpr uint32_t kConnectTimeoutMs = 15000;
constexpr int kMaxSavedNetworks = 8;
constexpr uint32_t kReconnectMaxDelayMs = 60000;
constexpr uint32_t kDiscoveryInitialDelayMs = 5000;
constexpr uint32_t kFastRetryDelayMs = 1000;
constexpr uint32_t kRadioSettleMs = 250;
constexpr uint32_t kScanStartTimeoutMs = 2500;
constexpr uint32_t kScanStartRetryMs = 200;
constexpr uint32_t kScanMaxMsPerChannel = 200;

// --- Persistence Helpers ---

int findSavedNetworkSlot(Preferences& prefs, uint8_t count, const char* ssid) {
    char key[4];
    for (uint8_t i = 0; i < count; i++) {
        snprintf(key, sizeof(key), "s%u", i);
        if (prefs.getString(key, "").equals(ssid)) return i;
    }
    return -1;
}

void saveNetworkCredential(const char* ssid, const char* password) {
    if (!ssid || ssid[0] == '\0') return;
    Preferences prefs;
    if (!prefs.begin("wifi_net", false)) return;

    uint8_t count = prefs.getUChar("cnt", 0);
    int slot = findSavedNetworkSlot(prefs, count, ssid);

    if (slot < 0) {
        if (count < kMaxSavedNetworks) {
            slot = count++;
            prefs.putUChar("cnt", count);
        } else {
            slot = prefs.getUChar("next", 0);
            prefs.putUChar("next", (slot + 1) % kMaxSavedNetworks);
        }
        char ssidKey[4];
        snprintf(ssidKey, sizeof(ssidKey), "s%d", slot);
        prefs.putString(ssidKey, ssid);
    }
    char passKey[4];
    snprintf(passKey, sizeof(passKey), "p%d", slot);
    prefs.putString(passKey, password ? password : "");
    uint32_t successSequence = prefs.getUInt("seq", 0) + 1;
    if (successSequence == 0) successSequence = 1;
    prefs.putUInt("seq", successSequence);
    char successKey[4];
    snprintf(successKey, sizeof(successKey), "u%d", slot);
    prefs.putUInt(successKey, successSequence);
    prefs.putUChar("last", slot);
    prefs.end();
}

bool removeSavedNetworkCredential(const char* ssid) {
    if (!ssid || ssid[0] == '\0') return false;
    Preferences prefs;
    if (!prefs.begin("wifi_net", false)) return false;

    const uint8_t count = min(prefs.getUChar("cnt", 0),
                              static_cast<uint8_t>(kMaxSavedNetworks));
    const int slot = findSavedNetworkSlot(prefs, count, ssid);
    if (slot < 0) {
        prefs.end();
        return false;
    }

    const uint8_t oldLast = prefs.getUChar("last", kMaxSavedNetworks);
    for (uint8_t i = static_cast<uint8_t>(slot); i + 1 < count; ++i) {
        char ssidKey[4], passKey[4], successKey[4];
        char nextSsidKey[4], nextPassKey[4], nextSuccessKey[4];
        snprintf(ssidKey, sizeof(ssidKey), "s%u", i);
        snprintf(passKey, sizeof(passKey), "p%u", i);
        snprintf(successKey, sizeof(successKey), "u%u", i);
        snprintf(nextSsidKey, sizeof(nextSsidKey), "s%u", i + 1);
        snprintf(nextPassKey, sizeof(nextPassKey), "p%u", i + 1);
        snprintf(nextSuccessKey, sizeof(nextSuccessKey), "u%u", i + 1);
        prefs.putString(ssidKey, prefs.getString(nextSsidKey, ""));
        prefs.putString(passKey, prefs.getString(nextPassKey, ""));
        prefs.putUInt(successKey, prefs.getUInt(nextSuccessKey, 0));
    }

    char finalSsidKey[4], finalPassKey[4], finalSuccessKey[4];
    snprintf(finalSsidKey, sizeof(finalSsidKey), "s%u", count - 1);
    snprintf(finalPassKey, sizeof(finalPassKey), "p%u", count - 1);
    snprintf(finalSuccessKey, sizeof(finalSuccessKey), "u%u", count - 1);
    prefs.remove(finalSsidKey);
    prefs.remove(finalPassKey);
    prefs.remove(finalSuccessKey);
    prefs.putUChar("cnt", count - 1);
    prefs.putUChar("next", 0);

    if (oldLast == slot || count == 1) {
        prefs.remove("last");
    } else if (oldLast < count) {
        prefs.putUChar("last", oldLast > slot ? oldLast - 1 : oldLast);
    }
    prefs.end();
    return true;
}

void stopMdns() {
    if (mdnsRunning) {
        MDNS.end();
        mdnsRunning = false;
    }
}

void ensureMdns() {
    if (mdnsRunning || (!apRunning && WiFi.status() != WL_CONNECTED)) return;
    if (!MDNS.begin(device_identity::getHostname())) return;
    // ESPmDNS adds the leading underscores in the DNS-SD record, yielding
    // _viewe-ota._tcp.local.
    MDNS.addService("viewe-ota", "tcp", 80);
    MDNS.addServiceTxt("viewe-ota", "tcp", "id", device_identity::getDeviceId());
    MDNS.addServiceTxt("viewe-ota", "tcp", "hardware_id", device_identity::getHardwareId());
    mdnsRunning = true;
}

bool timeReached(uint32_t deadline, uint32_t now = millis()) {
    return deadline != 0 && static_cast<int32_t>(now - deadline) >= 0;
}

bool scanBusy() {
    return scanState == ScanState::Starting || scanState == ScanState::Running;
}

bool isSuppressedSsid(const char* ssid) {
    for (size_t i = 0; i < suppressedSsidCount; ++i) {
        if (strcmp(suppressedSsids[i], ssid) == 0) return true;
    }
    return false;
}

void suppressSsid(const char* ssid) {
    if (!ssid || ssid[0] == '\0' || isSuppressedSsid(ssid) ||
        suppressedSsidCount >= kMaxSavedNetworks) return;
    strncpy(suppressedSsids[suppressedSsidCount], ssid,
            sizeof(suppressedSsids[0]) - 1);
    suppressedSsids[suppressedSsidCount][sizeof(suppressedSsids[0]) - 1] = '\0';
    ++suppressedSsidCount;
}

void unsuppressSsid(const char* ssid) {
    for (size_t i = 0; i < suppressedSsidCount; ++i) {
        if (strcmp(suppressedSsids[i], ssid) != 0) continue;
        for (size_t j = i; j + 1 < suppressedSsidCount; ++j) {
            memcpy(suppressedSsids[j], suppressedSsids[j + 1],
                   sizeof(suppressedSsids[j]));
        }
        --suppressedSsidCount;
        suppressedSsids[suppressedSsidCount][0] = '\0';
        return;
    }
}

bool allSavedNetworksSuppressed() {
    const int count = getSavedNetworkCount();
    if (count <= 0) return false;
    for (int i = 0; i < count; ++i) {
        char ssid[33];
        if (getSavedNetwork(i, ssid, sizeof(ssid)) && !isSuppressedSsid(ssid)) {
            return false;
        }
    }
    return true;
}

void setTarget(const char* ssid, const char* password) {
    strncpy(targetSsid, ssid ? ssid : "", sizeof(targetSsid) - 1);
    targetSsid[sizeof(targetSsid) - 1] = '\0';
    strncpy(lastPassword, password ? password : "", sizeof(lastPassword) - 1);
    lastPassword[sizeof(lastPassword) - 1] = '\0';
}

void scheduleRecoveryAction(PendingRecoveryAction action, uint32_t delayMs,
                            ConnectionFailure failure) {
    pendingRecoveryAction = action;
    connectionStartPending = false;
    connectionFailure = failure;
    nextReconnectAt = millis() + max(delayMs, 1UL);
    connectionPhase = ConnectionPhase::RetryWaiting;
    recoveryState = action == PendingRecoveryAction::FastRetry
                        ? RecoveryState::FastRetry
                        : RecoveryState::Waiting;
}

void stopAutomaticRecovery(ConnectionFailure failure = ConnectionFailure::None) {
    retryEnabled = false;
    pendingRecoveryAction = PendingRecoveryAction::None;
    nextReconnectAt = 0;
    recoveryState = RecoveryState::Disabled;
    connectionFailure = failure;
    connectionPhase = connectionFailureNeedsUserAction(failure)
                          ? ConnectionPhase::ActionRequired
                          : ConnectionPhase::Idle;
}

void scheduleDiscovery(ConnectionFailure failure, bool advanceBackoff) {
    if (!retryEnabled || getSavedNetworkCount() <= 0) {
        stopAutomaticRecovery(failure);
        return;
    }
    if (allSavedNetworksSuppressed()) {
        pendingRecoveryAction = PendingRecoveryAction::None;
        nextReconnectAt = 0;
        recoveryState = RecoveryState::Blocked;
        connectionFailure = failure;
        connectionPhase = ConnectionPhase::ActionRequired;
        return;
    }

    const uint32_t delay = advanceBackoff ? reconnectDelayMs : kRadioSettleMs;
    scheduleRecoveryAction(PendingRecoveryAction::Discover, delay, failure);
    if (advanceBackoff) {
        reconnectDelayMs = nextDiscoveryDelay(reconnectDelayMs,
                                              kReconnectMaxDelayMs);
    }
}

void saveApSettings(const char* ssid, bool secure, const char* password, bool enabled) {
    Preferences prefs;
    if (!prefs.begin("wifi_ap", false)) return;
    prefs.putString("ssid", ssid);
    prefs.putBool("secure", secure);
    prefs.putString("pass", secure ? password : "");
    prefs.putBool("enabled", enabled);
    prefs.end();
}

void saveApEnabled(bool enabled) {
    Preferences prefs;
    if (!prefs.begin("wifi_ap", false)) return;
    prefs.putBool("enabled", enabled);
    prefs.end();
}

bool loadApSettings(char* ssidOut, size_t ssidLen, bool& secureOut,
                    char* passOut, size_t passLen, bool& enabledOut) {
    ssidOut[0] = '\0';
    passOut[0] = '\0';
    secureOut = true;
    enabledOut = false;
    Preferences prefs;
    if (!prefs.begin("wifi_ap", true)) return false;
    const String ssid = prefs.getString("ssid", "");
    const String password = prefs.getString("pass", "");
    strncpy(ssidOut, ssid.c_str(), ssidLen - 1);
    ssidOut[ssidLen - 1] = '\0';
    secureOut = prefs.getBool("secure", true);
    strncpy(passOut, password.c_str(), passLen - 1);
    passOut[passLen - 1] = '\0';
    enabledOut = prefs.getBool("enabled", false);
    prefs.end();
    return ssidOut[0] != '\0';
}

void makeDefaultApSettings(char* ssidOut, size_t ssidLen, bool& secureOut,
                           char* passOut, size_t passLen) {
    if (POWER_METER_DEFAULT_AP_SSID[0] != '\0') {
        strncpy(ssidOut, POWER_METER_DEFAULT_AP_SSID, ssidLen - 1);
        ssidOut[ssidLen - 1] = '\0';
    } else {
        const char* deviceId = device_identity::getDeviceId();
        const size_t length = strlen(deviceId);
        const char first = length >= 2 ? deviceId[length - 2] : '0';
        const char second = length >= 1 ? deviceId[length - 1] : '0';
        snprintf(ssidOut, ssidLen, "meter%c%c", first, second);
    }

    strncpy(passOut, POWER_METER_DEFAULT_AP_PASSWORD, passLen - 1);
    passOut[passLen - 1] = '\0';
    secureOut = strlen(passOut) >= 8;
    if (passOut[0] != '\0' && !secureOut) {
        Serial.println("network: configured default AP password is shorter than 8 characters; starting open AP");
        passOut[0] = '\0';
    }
}

void ntpSyncCb(struct timeval*) { ntpSyncObserved = true; }

void internetProbeTask(void*) {
    bool reachable = false;
    NetworkClient client;
    if (WiFi.status() == WL_CONNECTED &&
        client.connect(kConnectivityHost, 80, kInternetProbeTimeoutMs)) {
        client.print("GET /generate_204 HTTP/1.1\r\nHost: connectivitycheck.gstatic.com\r\nConnection: close\r\n\r\n");
        const uint32_t deadline = millis() + kInternetProbeTimeoutMs;
        char statusLine[48] = {};
        size_t used = 0;
        bool responseReceived = false;
        while (static_cast<int32_t>(millis() - deadline) < 0 && used + 1 < sizeof(statusLine)) {
            while (client.available() && used + 1 < sizeof(statusLine)) {
                const char ch = static_cast<char>(client.read());
                if (ch == '\n') {
                    statusLine[used] = '\0';
                    reachable = strstr(statusLine, " 204 ") != nullptr;
                    responseReceived = true;
                    break;
                }
                if (ch != '\r') statusLine[used++] = ch;
            }
            if (responseReceived) break;
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    client.stop();
    internetProbeResult = reachable ? 1 : 0;
    vTaskDelete(nullptr);
}

void scheduleInternetProbe(bool immediate = false) {
    if (internetProbeInFlight || scanBusy() || WiFi.status() != WL_CONNECTED) return;
    const uint32_t now = millis();
    if (!immediate && now - lastInternetProbeMs < kInternetProbeIntervalMs) return;
    internetProbeResult = -1;
    internetProbeInFlight = true;
    lastInternetProbeMs = now;
    if (xTaskCreate(internetProbeTask, "internet_probe", 4096, nullptr, 1, nullptr) != pdPASS) {
        internetProbeInFlight = false;
    }
}

void applyInternetEvidence() {
    const bool canUpdateConnectedState = currentState == NetworkState::ConnectedStaLocal ||
                                         currentState == NetworkState::ConnectedStaInternet;
    if (ntpSyncObserved) {
        ntpSyncObserved = false;
        // The SNTP callback runs on the networking stack's task. Persisting
        // to LittleFS there could race storage queries and stall that task, so
        // capture the already-synchronised system clock from the main loop.
        timeval now{};
        gettimeofday(&now, nullptr);
        const int64_t unixTimeMs = static_cast<int64_t>(now.tv_sec) * 1000LL + now.tv_usec / 1000;
        time_service::submitAnchor(unixTimeMs, time_service::AnchorSource::Ntp,
                                   time_service::utcOffsetMinutes(), 250);
        if (canUpdateConnectedState && WiFi.status() == WL_CONNECTED) {
            currentState = NetworkState::ConnectedStaInternet;
            consecutiveInternetProbeFailures = 0;
            lastInternetProbeMs = millis();
        }
    }
    if (!internetProbeInFlight || internetProbeResult < 0) return;
    const bool reachable = internetProbeResult == 1;
    internetProbeResult = -1;
    internetProbeInFlight = false;
    if (scanBusy()) return;
    if (canUpdateConnectedState && WiFi.status() == WL_CONNECTED) {
        if (reachable) {
            consecutiveInternetProbeFailures = 0;
            currentState = NetworkState::ConnectedStaInternet;
        } else if (++consecutiveInternetProbeFailures >= 2) {
            currentState = NetworkState::ConnectedStaLocal;
        }
    }
}

void wifiEventCb(arduino_event_id_t event, arduino_event_info_t info) {
    if (!wifiEventQueue) return;
    if (event != ARDUINO_EVENT_WIFI_STA_CONNECTED &&
        event != ARDUINO_EVENT_WIFI_STA_DISCONNECTED &&
        event != ARDUINO_EVENT_WIFI_STA_GOT_IP &&
        event != ARDUINO_EVENT_WIFI_STA_LOST_IP) return;

    WifiEventRecord record{event, 0};
    if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
        record.disconnectReason = info.wifi_sta_disconnected.reason;
    }
    xQueueSend(wifiEventQueue, &record, 0);
}

ConnectionFailure classifyDisconnect(uint8_t reason, bool wasConnected) {
    if (wasConnected) return ConnectionFailure::LinkLost;
    switch (reason) {
        case WIFI_REASON_NO_AP_FOUND:
        case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
            return ConnectionFailure::NetworkNotFound;
        case WIFI_REASON_AUTH_FAIL:
        case WIFI_REASON_802_1X_AUTH_FAILED:
            return ConnectionFailure::AuthenticationFailed;
        case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
        case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
        case WIFI_REASON_GROUP_CIPHER_INVALID:
        case WIFI_REASON_PAIRWISE_CIPHER_INVALID:
        case WIFI_REASON_AKMP_INVALID:
        case WIFI_REASON_CIPHER_SUITE_REJECTED:
        case WIFI_REASON_BAD_CIPHER_OR_AKM:
            return ConnectionFailure::IncompatibleSecurity;
        default:
            return ConnectionFailure::ConnectionFailed;
    }
}

void configureNetworkTime() {
    if (ntpConfigured) return;
    sntp_set_time_sync_notification_cb(ntpSyncCb);
    configTime(static_cast<long>(time_service::utcOffsetMinutes()) * 60L,
               0, kNtpServer1, kNtpServer2);
    ntpConfigured = true;
}

void markStationConnected() {
    if (WiFi.status() != WL_CONNECTED) return;
    if (currentState == NetworkState::ConnectedStaLocal ||
        currentState == NetworkState::ConnectedStaInternet) return;
    currentState = NetworkState::ConnectedStaLocal;
    connectionPhase = ConnectionPhase::Idle;
    connectionFailure = ConnectionFailure::None;
    expectedDisconnect = ExpectedDisconnect::None;
    connectionStartPending = false;
    pendingRecoveryAction = PendingRecoveryAction::None;
    nextReconnectAt = 0;
    reconnectDelayMs = kDiscoveryInitialDelayMs;
    retryEnabled = targetSsid[0] != '\0';
    recoveryState = retryEnabled ? RecoveryState::Idle : RecoveryState::Disabled;
    attemptOrigin = AttemptOrigin::None;
    if (targetSsid[0] != '\0') unsuppressSsid(targetSsid);
    saveNetworkCredential(targetSsid, lastPassword);
    configureNetworkTime();
    ensureMdns();
    scheduleInternetProbe(true);
    device_state::changed(device_state::Domain::Network);
}

void handleConnectionFailure(ConnectionFailure failure, bool wasConnected);

bool startConnectionAttempt() {
    connectionStartPending = false;
    if (!retryEnabled || targetSsid[0] == '\0' || scanBusy()) return false;

    expectedDisconnect = ExpectedDisconnect::None;
    WiFi.mode(apRunning ? WIFI_AP_STA : WIFI_STA);
    const wl_status_t result = lastPassword[0] != '\0'
                                   ? WiFi.begin(targetSsid, lastPassword)
                                   : WiFi.begin(targetSsid);
    if (result == WL_CONNECT_FAILED) {
        currentState = NetworkState::Disconnected;
        expectedDisconnect = ExpectedDisconnect::FailureCleanup;
        WiFi.disconnect(false, false);
        handleConnectionFailure(ConnectionFailure::ConnectionFailed, false);
        device_state::changed(device_state::Domain::Network);
        return false;
    }

    connectStartTime = millis();
    nextReconnectAt = 0;
    currentState = NetworkState::ConnectingSta;
    connectionPhase = ConnectionPhase::LookingForNetwork;
    connectionFailure = ConnectionFailure::None;
    device_state::changed(device_state::Domain::Network);
    if (WiFi.status() == WL_CONNECTED) markStationConnected();
    return true;
}

bool startNextRecoveryCandidate() {
    RecoveryCandidatePolicy policy[kMaxSavedNetworks];
    for (size_t i = 0; i < recoveryCandidateCount; ++i) {
        policy[i] = recoveryCandidates[i].policy;
    }
    const int selected = chooseRecoveryCandidate(policy, recoveryCandidateCount);
    if (selected < 0) {
        scheduleDiscovery(connectionFailure == ConnectionFailure::None
                              ? ConnectionFailure::NetworkNotFound
                              : connectionFailure,
                          true);
        return false;
    }

    RecoveryCandidate& candidate = recoveryCandidates[selected];
    candidate.policy.attempted = true;
    setTarget(candidate.ssid, candidate.password);
    attemptOrigin = AttemptOrigin::RecoveryCandidate;
    pendingRecoveryAction = PendingRecoveryAction::None;
    nextReconnectAt = 0;
    recoveryState = RecoveryState::TryingCandidate;
    connectionPhase = ConnectionPhase::LookingForNetwork;
    connectionFailure = ConnectionFailure::None;
    return startConnectionAttempt();
}

void prepareRecoveryCandidates() {
    recoveryCandidateCount = 0;
    const int savedCount = getSavedNetworkCount();
    for (int savedIndex = 0;
         savedIndex < savedCount && recoveryCandidateCount < kMaxSavedNetworks;
         ++savedIndex) {
        char ssid[33];
        if (!getSavedNetwork(savedIndex, ssid, sizeof(ssid)) ||
            isSuppressedSsid(ssid)) continue;

        int bestRssi = -1000;
        bool visible = false;
        for (int scanIndex = 0; scanIndex < scanCount; ++scanIndex) {
            if (WiFi.SSID(scanIndex) != ssid) continue;
            bestRssi = max(bestRssi, static_cast<int>(WiFi.RSSI(scanIndex)));
            visible = true;
        }
        if (!visible) continue;

        RecoveryCandidate& candidate = recoveryCandidates[recoveryCandidateCount++];
        candidate.policy = {true, false, false,
                            static_cast<uint8_t>(savedIndex), bestRssi};
        strncpy(candidate.ssid, ssid, sizeof(candidate.ssid) - 1);
        candidate.ssid[sizeof(candidate.ssid) - 1] = '\0';
        if (!getSavedPassword(ssid, candidate.password,
                              sizeof(candidate.password))) {
            candidate.password[0] = '\0';
        }
    }

    if (recoveryCandidateCount > 0) {
        scheduleRecoveryAction(PendingRecoveryAction::TryNextCandidate,
                               kRadioSettleMs, ConnectionFailure::None);
    } else {
        scheduleDiscovery(ConnectionFailure::NetworkNotFound, true);
    }
}

bool requestScan(uint8_t purpose) {
    scanPurpose |= purpose;
    if (scanBusy()) {
        if ((purpose & ScanPurposeRecovery) != 0 &&
            currentState == NetworkState::Disconnected) {
            recoveryState = RecoveryState::Discovering;
        }
        return true;
    }

    scanCount = 0;
    scanState = ScanState::Starting;
    scanStartDeadline = millis() + kScanStartTimeoutMs;
    nextScanStartAttempt = millis();
    nextReconnectAt = 0;
    pendingRecoveryAction = PendingRecoveryAction::None;

    if ((purpose & ScanPurposeRecovery) != 0 &&
        currentState == NetworkState::Disconnected) {
        recoveryState = RecoveryState::Discovering;
        connectionPhase = ConnectionPhase::RetryWaiting;
        setTarget("", "");
    }

    if (currentState == NetworkState::ConnectingSta || connectionStartPending) {
        currentState = NetworkState::Disconnected;
        connectionStartPending = false;
        if ((purpose & ScanPurposeRecovery) != 0) {
            recoveryState = RecoveryState::Discovering;
            setTarget("", "");
        }
        expectedDisconnect = ExpectedDisconnect::Scan;
        WiFi.disconnect(false, false);
        nextScanStartAttempt = millis() + kRadioSettleMs;
    }
    return true;
}

void finishScan(ScanState finalState, int resultCount = 0) {
    scanCount = finalState == ScanState::Succeeded ? max(resultCount, 0) : 0;
    scanState = finalState;
    ++scanGeneration;
    const uint8_t completedPurpose = scanPurpose;
    scanPurpose = ScanPurposeNone;

    if ((completedPurpose & ScanPurposeRecovery) != 0 && retryEnabled &&
        currentState == NetworkState::Disconnected) {
        if (finalState == ScanState::Succeeded) prepareRecoveryCandidates();
        else scheduleDiscovery(ConnectionFailure::ConnectionFailed, true);
    }
}

void updateScanOperation() {
    const uint32_t now = millis();
    if (scanState == ScanState::Starting && timeReached(nextScanStartAttempt, now)) {
        const int result = WiFi.scanNetworks(true, false, false,
                                             kScanMaxMsPerChannel);
        if (result == WIFI_SCAN_RUNNING) {
            scanState = ScanState::Running;
        } else if (result >= 0) {
            finishScan(ScanState::Succeeded, result);
        } else if (timeReached(scanStartDeadline, now)) {
            finishScan(ScanState::Failed);
        } else {
            nextScanStartAttempt = now + kScanStartRetryMs;
        }
    }

    if (scanState == ScanState::Running) {
        const int result = WiFi.scanComplete();
        if (result == WIFI_SCAN_RUNNING) return;
        finishScan(result >= 0 ? ScanState::Succeeded : ScanState::Failed,
                   result);
    }
}

void handleConnectionFailure(ConnectionFailure failure, bool wasConnected) {
    connectionFailure = failure;
    connectionStartPending = false;

    if (!retryEnabled) {
        stopAutomaticRecovery(failure);
        return;
    }

    if (wasConnected) {
        attemptOrigin = AttemptOrigin::FastRetry;
        scheduleRecoveryAction(PendingRecoveryAction::FastRetry,
                               kFastRetryDelayMs, failure);
        return;
    }

    if (getSavedNetworkCount() <= 0) {
        if ((attemptOrigin == AttemptOrigin::Manual ||
             attemptOrigin == AttemptOrigin::FastRetry) &&
            connectionFailureShouldRetry(failure) && targetSsid[0] != '\0') {
            scheduleRecoveryAction(PendingRecoveryAction::FastRetry,
                                   reconnectDelayMs, failure);
            reconnectDelayMs = nextDiscoveryDelay(reconnectDelayMs,
                                                  kReconnectMaxDelayMs);
        } else {
            stopAutomaticRecovery(failure);
        }
        return;
    }

    if (failureSuppressesAutomaticCandidate(failure)) suppressSsid(targetSsid);

    if (attemptOrigin == AttemptOrigin::RecoveryCandidate) {
        scheduleRecoveryAction(PendingRecoveryAction::TryNextCandidate,
                               kRadioSettleMs, failure);
    } else {
        scheduleDiscovery(failure, false);
    }
}

void handleDisconnectedEvent(uint8_t reason) {
    const ExpectedDisconnect expected = expectedDisconnect;
    if (expected != ExpectedDisconnect::None || reason == WIFI_REASON_ASSOC_LEAVE) {
        if (expected == ExpectedDisconnect::Reconfigure) {
            connectionStartPending = true;
            connectionStartNotBefore = millis() + kRadioSettleMs;
        }
        return;
    }

    if (currentState == NetworkState::Disconnected &&
        (connectionPhase == ConnectionPhase::RetryWaiting ||
         connectionPhase == ConnectionPhase::ActionRequired)) return;

    const bool wasConnected = currentState == NetworkState::ConnectedStaLocal ||
                              currentState == NetworkState::ConnectedStaInternet;
    currentState = NetworkState::Disconnected;
    stopMdns();
    const ConnectionFailure failure = classifyDisconnect(reason, wasConnected);
    expectedDisconnect = ExpectedDisconnect::FailureCleanup;
    WiFi.disconnect(false, false);
    handleConnectionFailure(failure, wasConnected);
    device_state::changed(device_state::Domain::Network);
}

void processWifiEvents() {
    if (!wifiEventQueue) return;
    WifiEventRecord record{};
    while (xQueueReceive(wifiEventQueue, &record, 0) == pdTRUE) {
        switch (record.id) {
            case ARDUINO_EVENT_WIFI_STA_CONNECTED:
                if (expectedDisconnect != ExpectedDisconnect::None) break;
                if (currentState == NetworkState::ConnectingSta) {
                    connectionPhase = ConnectionPhase::ObtainingIp;
                }
                break;
            case ARDUINO_EVENT_WIFI_STA_GOT_IP:
                if (expectedDisconnect != ExpectedDisconnect::None) break;
                markStationConnected();
                break;
            case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
                handleDisconnectedEvent(record.disconnectReason);
                break;
            case ARDUINO_EVENT_WIFI_STA_LOST_IP:
                if (currentState == NetworkState::ConnectedStaInternet) {
                    currentState = NetworkState::ConnectedStaLocal;
                }
                break;
            default:
                break;
        }
    }
}

} // namespace

void init() {
    device_identity::init();
    time_service::init();
    wifiEventQueue = xQueueCreateStatic(8, sizeof(WifiEventRecord),
                                        wifiEventQueueBuffer,
                                        &wifiEventQueueStorage);
    WiFi.onEvent(wifiEventCb);
    WiFi.setAutoReconnect(false);
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(device_identity::getHostname());
    const int savedNetworkCount = getSavedNetworkCount();
    char apSsid[33], apPassword[64];
    bool apSecure = true, apEnabled = false;
    const bool hasApSettings = loadApSettings(
        apSsid, sizeof(apSsid), apSecure, apPassword, sizeof(apPassword), apEnabled);
    if (hasApSettings && apEnabled) {
        WiFi.mode(WIFI_AP_STA);
        if (WiFi.softAP(apSsid, apSecure ? apPassword : nullptr)) {
            apRunning = true;
            ensureMdns();
        }
    } else if (!hasApSettings && savedNetworkCount == 0) {
        makeDefaultApSettings(apSsid, sizeof(apSsid), apSecure,
                              apPassword, sizeof(apPassword));
        WiFi.mode(WIFI_AP_STA);
        if (WiFi.softAP(apSsid, apSecure ? apPassword : nullptr)) {
            apRunning = true;
            saveApSettings(apSsid, apSecure, apPassword, true);
            ensureMdns();
            Serial.printf("network: started default AP %s (%s)\n", apSsid,
                          apSecure ? "secured" : "open");
        } else {
            Serial.println("network: failed to start default AP");
        }
    }
    if (savedNetworkCount > 0) {
        retryEnabled = true;
        recoveryState = RecoveryState::Discovering;
        requestScan(ScanPurposeRecovery);
    }
    reportWebAddressesIfChanged();
}

void update() {
    processWifiEvents();
    applyInternetEvidence();
    updateScanOperation();

    if (connectionStartPending && !scanBusy() &&
        timeReached(connectionStartNotBefore)) {
        startConnectionAttempt();
    }

    if (currentState == NetworkState::Disconnected && !scanBusy() &&
        !connectionStartPending && pendingRecoveryAction != PendingRecoveryAction::None &&
        timeReached(nextReconnectAt)) {
        const PendingRecoveryAction action = pendingRecoveryAction;
        pendingRecoveryAction = PendingRecoveryAction::None;
        nextReconnectAt = 0;
        switch (action) {
            case PendingRecoveryAction::FastRetry:
                attemptOrigin = AttemptOrigin::FastRetry;
                recoveryState = RecoveryState::FastRetry;
                startConnectionAttempt();
                break;
            case PendingRecoveryAction::TryNextCandidate:
                startNextRecoveryCandidate();
                break;
            case PendingRecoveryAction::Discover:
                requestScan(ScanPurposeRecovery);
                break;
            default:
                break;
        }
    }

    switch (currentState) {
        case NetworkState::ConnectingSta: {
            if (WiFi.status() == WL_CONNECTED) {
                markStationConnected();
            } else if (millis() - connectStartTime > kConnectTimeoutMs) {
                currentState = NetworkState::Disconnected;
                expectedDisconnect = ExpectedDisconnect::FailureCleanup;
                WiFi.disconnect(false, false);
                handleConnectionFailure(ConnectionFailure::TimedOut, false);
                device_state::changed(device_state::Domain::Network);
            }
            break;
        }
        case NetworkState::ConnectedStaLocal: {
            if (WiFi.status() != WL_CONNECTED) {
                currentState = NetworkState::Disconnected;
                stopMdns();
                expectedDisconnect = ExpectedDisconnect::FailureCleanup;
                WiFi.disconnect(false, false);
                handleConnectionFailure(ConnectionFailure::LinkLost, true);
                device_state::changed(device_state::Domain::Network);
                break;
            }
            if (!scanBusy()) scheduleInternetProbe();
            break;
        }
        case NetworkState::ConnectedStaInternet: {
            if (WiFi.status() != WL_CONNECTED) {
                currentState = NetworkState::Disconnected;
                stopMdns();
                expectedDisconnect = ExpectedDisconnect::FailureCleanup;
                WiFi.disconnect(false, false);
                handleConnectionFailure(ConnectionFailure::LinkLost, true);
                device_state::changed(device_state::Domain::Network);
            } else if (!scanBusy()) {
                scheduleInternetProbe();
            }
            break;
        }
        case NetworkState::Disconnected:
            ensureMdns();
            break;
        default:
            break;
    }
    reportWebAddressesIfChanged();
}

bool connectTo(const char* ssid, const char* password) {
    if (!ssid || ssid[0] == '\0' || scanBusy()) return false;
    const bool alreadyConnected =
        (currentState == NetworkState::ConnectedStaLocal ||
         currentState == NetworkState::ConnectedStaInternet) &&
        strcmp(targetSsid, ssid) == 0;
    if (alreadyConnected) return true;

    setTarget(ssid, password);
    unsuppressSsid(ssid);

    retryEnabled = true;
    reconnectDelayMs = kDiscoveryInitialDelayMs;
    nextReconnectAt = 0;
    pendingRecoveryAction = PendingRecoveryAction::None;
    attemptOrigin = AttemptOrigin::Manual;
    recoveryState = RecoveryState::TryingCandidate;
    connectionFailure = ConnectionFailure::None;
    connectionPhase = ConnectionPhase::LookingForNetwork;

    if (currentState != NetworkState::Disconnected ||
        WiFi.status() == WL_CONNECTED) {
        currentState = NetworkState::Disconnected;
        stopMdns();
        expectedDisconnect = ExpectedDisconnect::Reconfigure;
        connectionStartPending = true;
        connectionStartNotBefore = millis() + kRadioSettleMs;
        WiFi.disconnect(false, false);
        device_state::changed(device_state::Domain::Network);
        return true;
    }
    if (expectedDisconnect != ExpectedDisconnect::None) {
        expectedDisconnect = ExpectedDisconnect::Reconfigure;
        connectionStartPending = true;
        connectionStartNotBefore = millis() + kRadioSettleMs;
        device_state::changed(device_state::Domain::Network);
        return true;
    }
    return startConnectionAttempt();
}

void disconnect() {
    stopAutomaticRecovery();
    connectionStartPending = false;
    reconnectDelayMs = kDiscoveryInitialDelayMs;
    recoveryCandidateCount = 0;
    attemptOrigin = AttemptOrigin::None;
    targetSsid[0] = '\0';
    lastPassword[0] = '\0';
    connectionFailure = ConnectionFailure::None;
    connectionPhase = ConnectionPhase::Idle;
    currentState = NetworkState::Disconnected;
    expectedDisconnect = ExpectedDisconnect::Manual;
    WiFi.disconnect(false, false);
    stopMdns();
    device_state::changed(device_state::Domain::Network);
}

bool startAp(const char* ssid, const char* password, bool secure) {
    WiFi.mode(WIFI_AP_STA); // Forces hardware to support both
    if (WiFi.softAP(ssid, secure ? password : nullptr)) {
        apRunning = true;
        saveApSettings(ssid, secure, password, true);
        ensureMdns();
        device_state::changed(device_state::Domain::Network);
        return true;
    }
    return false;
}

void stopAp() {
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA); // Revert to STA-only to save power/cycles
    apRunning = false;
    saveApEnabled(false);
    if (WiFi.status() != WL_CONNECTED) stopMdns();
    device_state::changed(device_state::Domain::Network);
}

bool isApEnabled() {
    return apRunning;
}

const char* getStaIpAddress() {
    static char ip[16];
    snprintf(ip, sizeof(ip), "%s", WiFi.localIP().toString().c_str());
    return ip;
}

const char* getApIpAddress() {
    static char ip[16];
    snprintf(ip, sizeof(ip), "%s", WiFi.softAPIP().toString().c_str());
    return ip;
}

const char* getHostname() { return device_identity::getHostname(); }

void restartMdns() {
    WiFi.setHostname(device_identity::getHostname());
    stopMdns();
    ensureMdns();
}

bool scanNetworks() {
    uint8_t purpose = ScanPurposeUser;
    if (retryEnabled && currentState == NetworkState::Disconnected) {
        purpose |= ScanPurposeRecovery;
    }
    return requestScan(purpose);
}

NetworkState getState() {
    return currentState;
}

ConnectionPhase getConnectionPhase() { return connectionPhase; }

ConnectionFailure getConnectionFailure() { return connectionFailure; }

RecoveryState getRecoveryState() { return recoveryState; }

const char* getCurrentSsid() {
    return targetSsid;
}

uint32_t getReconnectSecondsRemaining() {
    if (connectionPhase != ConnectionPhase::RetryWaiting ||
        nextReconnectAt == 0) return 0;
    const int32_t remaining = static_cast<int32_t>(nextReconnectAt - millis());
    return remaining <= 0 ? 0 : static_cast<uint32_t>(remaining + 999) / 1000;
}

int getRssi() {
    return WiFi.RSSI();
}

int getScanResultCount() {
    return scanCount;
}

bool getScanResult(int index, char* ssidOut, size_t ssidLen, bool& secureOut, int& rssiOut) {
    if (index < 0 || index >= scanCount) return false;
    strncpy(ssidOut, WiFi.SSID(index).c_str(), ssidLen - 1);
    ssidOut[ssidLen - 1] = '\0';
    secureOut = (WiFi.encryptionType(index) != WIFI_AUTH_OPEN);
    rssiOut = WiFi.RSSI(index);
    return true;
}

int getApClientCount() {
    return WiFi.softAPgetStationNum();
}

bool getApClientMac(int index, char* macStrOut, size_t maxLen) {
    wifi_sta_list_t staList;
    if (esp_wifi_ap_get_sta_list(&staList) != ESP_OK || index >= staList.num) return false;
    const uint8_t* mac = staList.sta[index].mac;
    snprintf(macStrOut, maxLen, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return true;
}

int getSavedNetworkCount() {
    Preferences prefs;
    if (!prefs.begin("wifi_net", true)) return 0;
    const int count = min(prefs.getUChar("cnt", 0),
                          static_cast<uint8_t>(kMaxSavedNetworks));
    prefs.end();
    return count;
}

bool getSavedNetwork(int index, char* ssidOut, size_t ssidLen) {
    if (!ssidOut || ssidLen == 0) return false;
    ssidOut[0] = '\0';
    Preferences prefs;
    if (!prefs.begin("wifi_net", true)) return false;
    const uint8_t count = min(prefs.getUChar("cnt", 0),
                              static_cast<uint8_t>(kMaxSavedNetworks));
    if (index < 0 || index >= count) {
        prefs.end();
        return false;
    }

    const uint8_t last = prefs.getUChar("last", kMaxSavedNetworks);
    uint8_t slots[kMaxSavedNetworks];
    uint32_t success[kMaxSavedNetworks];
    for (uint8_t i = 0; i < count; ++i) {
        slots[i] = i;
        char key[4];
        snprintf(key, sizeof(key), "u%u", i);
        success[i] = prefs.getUInt(key, 0);
    }
    for (uint8_t i = 0; i < count; ++i) {
        for (uint8_t j = i + 1; j < count; ++j) {
            const uint8_t a = slots[i];
            const uint8_t b = slots[j];
            const bool bPreferred = b == last && a != last;
            const bool bNewer = a != last && b != last &&
                                (success[b] > success[a] ||
                                 (success[b] == success[a] && b < a));
            if (bPreferred || bNewer) {
                slots[i] = b;
                slots[j] = a;
            }
        }
    }
    const uint8_t slot = slots[index];

    char key[4];
    snprintf(key, sizeof(key), "s%u", slot);
    const String ssid = prefs.getString(key, "");
    prefs.end();
    if (ssid.isEmpty()) return false;
    strncpy(ssidOut, ssid.c_str(), ssidLen - 1);
    ssidOut[ssidLen - 1] = '\0';
    return true;
}

bool connectSavedNetwork(const char* ssid) {
    char password[64];
    if (!getSavedPassword(ssid, password, sizeof(password))) return false;
    return connectTo(ssid, password);
}

bool forgetSavedNetwork(const char* ssid) {
    if (!ssid || ssid[0] == '\0') return false;
    const bool isTarget = strcmp(targetSsid, ssid) == 0;
    if (!removeSavedNetworkCredential(ssid)) return false;
    unsuppressSsid(ssid);
    for (size_t i = 0; i < recoveryCandidateCount; ++i) {
        if (strcmp(recoveryCandidates[i].ssid, ssid) == 0) {
            recoveryCandidates[i].policy.suppressed = true;
        }
    }
    if (isTarget) {
        currentState = NetworkState::Disconnected;
        connectionStartPending = false;
        expectedDisconnect = ExpectedDisconnect::FailureCleanup;
        WiFi.disconnect(false, false);
        stopMdns();
        setTarget("", "");
        attemptOrigin = AttemptOrigin::None;
    }

    if (getSavedNetworkCount() <= 0) {
        stopAutomaticRecovery();
        setTarget("", "");
    } else if (isTarget ||
               (currentState == NetworkState::Disconnected && retryEnabled && !scanBusy())) {
        retryEnabled = true;
        reconnectDelayMs = kDiscoveryInitialDelayMs;
        recoveryCandidateCount = 0;
        scheduleDiscovery(ConnectionFailure::None, false);
    }
    device_state::changed(device_state::Domain::Network);
    return true;
}

bool getSavedPassword(const char* ssid, char* passOut, size_t maxLen) {
    Preferences prefs;
    if (!prefs.begin("wifi_net", true)) return false;
    uint8_t count = prefs.getUChar("cnt", 0);
    int slot = findSavedNetworkSlot(prefs, count, ssid);
    if (slot >= 0) {
        char key[4];
        snprintf(key, sizeof(key), "p%d", slot);
        String pass = prefs.getString(key, "");
        strncpy(passOut, pass.c_str(), maxLen - 1);
        passOut[maxLen - 1] = '\0';
        prefs.end();
        return true;
    }
    prefs.end();
    return false;
}

void getSavedApSettings(char* ssidOut, size_t ssidLen, bool& secureOut, char* passOut, size_t passLen) {
    bool enabled = false;
    loadApSettings(ssidOut, ssidLen, secureOut, passOut, passLen, enabled);
}

ScanState getScanState() { return scanState; }

uint32_t getScanGeneration() { return scanGeneration; }

bool clearSavedCredentials() {
    Preferences networks;
    Preferences accessPoint;
    const bool networksOpen = networks.begin("wifi_net", false);
    const bool accessPointOpen = accessPoint.begin("wifi_ap", false);
    if (!networksOpen || !accessPointOpen) {
        if (networksOpen) networks.end();
        if (accessPointOpen) accessPoint.end();
        return false;
    }
    const bool cleared = networks.clear() && accessPoint.clear();
    networks.end();
    accessPoint.end();
    stopAutomaticRecovery();
    connectionStartPending = false;
    reconnectDelayMs = kDiscoveryInitialDelayMs;
    recoveryCandidateCount = 0;
    suppressedSsidCount = 0;
    targetSsid[0] = '\0';
    lastPassword[0] = '\0';
    currentState = NetworkState::Disconnected;
    connectionPhase = ConnectionPhase::Idle;
    connectionFailure = ConnectionFailure::None;
    // Also remove the Wi-Fi driver's remembered station configuration.
    WiFi.disconnect(true, true);
    return cleared;
}

} // namespace network_manager
