#include "network_manager.h"
#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include <NetworkClient.h>
#include <esp_sntp.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <time.h>
#include "device/device_identity.h"

namespace network_manager {
namespace {

NetworkState currentState = NetworkState::Disconnected;
uint32_t connectStartTime = 0;
char targetSsid[33] = {0};
char lastPassword[64] = {0};
bool apRunning = false; // Add this discrete AP state flag
bool mdnsRunning = false;
uint32_t nextReconnectAt = 0;
uint32_t reconnectDelayMs = 1000;

// Scan state
int scanCount = 0;
NetworkState stateBeforeScan = NetworkState::Disconnected;

// SNTP configuration
constexpr char kNtpServer1[] = "pool.ntp.org";
constexpr char kNtpServer2[] = "time.nist.gov";
constexpr char kTimezonePosix[] = "MST7MDT,M3.2.0,M11.1.0";
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
    prefs.putUChar("last", slot);
    prefs.end();
}

bool loadLastNetwork(char* ssidOut, size_t ssidLen, char* passOut, size_t passLen) {
    Preferences prefs;
    if (!prefs.begin("wifi_net", true)) return false;
    uint8_t count = prefs.getUChar("cnt", 0);
    uint8_t slot = prefs.getUChar("last", kMaxSavedNetworks);
    if (slot >= count) {
        prefs.end();
        return false;
    }
    char ssidKey[4], passKey[4];
    snprintf(ssidKey, sizeof(ssidKey), "s%u", slot);
    snprintf(passKey, sizeof(passKey), "p%u", slot);
    String ssid = prefs.getString(ssidKey, "");
    String password = prefs.getString(passKey, "");
    prefs.end();
    if (ssid.isEmpty()) return false;
    strncpy(ssidOut, ssid.c_str(), ssidLen - 1);
    ssidOut[ssidLen - 1] = '\0';
    strncpy(passOut, password.c_str(), passLen - 1);
    passOut[passLen - 1] = '\0';
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

void scheduleReconnect() {
    nextReconnectAt = millis() + reconnectDelayMs;
    reconnectDelayMs = min(reconnectDelayMs * 2, kReconnectMaxDelayMs);
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
    if (internetProbeInFlight || WiFi.status() != WL_CONNECTED) return;
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
    if (canUpdateConnectedState && WiFi.status() == WL_CONNECTED) {
        if (reachable) {
            consecutiveInternetProbeFailures = 0;
            currentState = NetworkState::ConnectedStaInternet;
        } else if (++consecutiveInternetProbeFailures >= 2) {
            currentState = NetworkState::ConnectedStaLocal;
        }
    }
}

} // namespace

void init() {
    device_identity::init();
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(device_identity::getHostname());
    WiFi.disconnect(false);
    char apSsid[33], apPassword[64];
    bool apSecure = true, apEnabled = false;
    if (loadApSettings(apSsid, sizeof(apSsid), apSecure, apPassword, sizeof(apPassword), apEnabled) && apEnabled) {
        WiFi.mode(WIFI_AP_STA);
        if (WiFi.softAP(apSsid, apSecure ? apPassword : nullptr)) {
            apRunning = true;
            ensureMdns();
        }
    }
    char ssid[33], password[64];
    if (loadLastNetwork(ssid, sizeof(ssid), password, sizeof(password))) {
        connectTo(ssid, password);
    }
}

void update() {
    applyInternetEvidence();
    switch (currentState) {
        case NetworkState::Scanning: {
            int n = WiFi.scanComplete();
            if (n != WIFI_SCAN_RUNNING) {
                scanCount = (n < 0) ? 0 : n;
                currentState = WiFi.status() == WL_CONNECTED ? stateBeforeScan : NetworkState::Disconnected;
                if (currentState == NetworkState::Disconnected && targetSsid[0] != '\0') {
                    scheduleReconnect();
                }
            }
            break;
        }
        case NetworkState::ConnectingSta: {
            if (WiFi.status() == WL_CONNECTED) {
                currentState = NetworkState::ConnectedStaLocal;

                saveNetworkCredential(targetSsid, lastPassword);

                if (!ntpConfigured) {
                    sntp_set_time_sync_notification_cb(ntpSyncCb);
                    configTzTime(kTimezonePosix, kNtpServer1, kNtpServer2);
                    ntpConfigured = true;
                }
                reconnectDelayMs = 1000;
                ensureMdns();
                scheduleInternetProbe(true);
            } else if (millis() - connectStartTime > kConnectTimeoutMs) {
                WiFi.disconnect();
                currentState = NetworkState::Disconnected;
                scheduleReconnect();
            }
            break;
        }
        case NetworkState::ConnectedStaLocal: {
            if (WiFi.status() != WL_CONNECTED) {
                currentState = NetworkState::Disconnected;
                stopMdns();
                scheduleReconnect();
                break;
            }
            scheduleInternetProbe();
            break;
        }
        case NetworkState::ConnectedStaInternet: {
            if (WiFi.status() != WL_CONNECTED) {
                currentState = NetworkState::Disconnected;
                stopMdns();
                scheduleReconnect();
            } else {
                scheduleInternetProbe();
            }
            break;
        }
        case NetworkState::Disconnected:
            ensureMdns();
            if (targetSsid[0] != '\0' && nextReconnectAt != 0 &&
                static_cast<int32_t>(millis() - nextReconnectAt) >= 0) {
                connectTo(targetSsid, lastPassword);
            }
            break;
        default:
            break;
    }
}

void connectTo(const char* ssid, const char* password) {
    // REMOVED: if (currentState == NetworkState::ApRunning) stopAp();

    // Ensure the radio mode supports our concurrent requirements
    if (apRunning) {
        WiFi.mode(WIFI_AP_STA);
    } else {
        WiFi.mode(WIFI_STA);
    }

    strncpy(targetSsid, ssid, sizeof(targetSsid) - 1);
    targetSsid[sizeof(targetSsid) - 1] = '\0';

    if (password && password[0] != '\0') {
        strncpy(lastPassword, password, sizeof(lastPassword) - 1);
        lastPassword[sizeof(lastPassword) - 1] = '\0';
        WiFi.begin(ssid, password);
    } else {
        lastPassword[0] = '\0';
        WiFi.begin(ssid);
    }

    connectStartTime = millis();
    nextReconnectAt = 0;
    currentState = NetworkState::ConnectingSta;
}

void startAp(const char* ssid, const char* password, bool secure) {
    WiFi.mode(WIFI_AP_STA); // Forces hardware to support both
    if (WiFi.softAP(ssid, secure ? password : nullptr)) {
        apRunning = true;
        saveApSettings(ssid, secure, password, true);
        ensureMdns();
    }
}

void stopAp() {
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA); // Revert to STA-only to save power/cycles
    apRunning = false;
    saveApEnabled(false);
    if (WiFi.status() != WL_CONNECTED) stopMdns();
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

void scanNetworks() {
    stateBeforeScan = currentState;
    WiFi.scanNetworks(true); // async
    currentState = NetworkState::Scanning;
}

NetworkState getState() {
    return currentState;
}

const char* getCurrentSsid() {
    return targetSsid;
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
    // Also remove the Wi-Fi driver's remembered station configuration.
    WiFi.disconnect(true, true);
    return cleared;
}

} // namespace network_manager
