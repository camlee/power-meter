#include "network_manager.h"
#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <Preferences.h>
#include <time.h>

namespace network_manager {
namespace {

NetworkState currentState = NetworkState::Disconnected;
uint32_t connectStartTime = 0;
char targetSsid[33] = {0};
char lastPassword[64] = {0};
bool apRunning = false; // Add this discrete AP state flag

// Scan state
int scanCount = 0;

// SNTP configuration
constexpr char kNtpServer1[] = "pool.ntp.org";
constexpr char kNtpServer2[] = "time.nist.gov";
constexpr char kTimezonePosix[] = "MST7MDT,M3.2.0,M11.1.0";
bool ntpConfigured = false;

constexpr uint32_t kConnectTimeoutMs = 15000;
constexpr int kMaxSavedNetworks = 8;

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
    if (!ssid || ssid[0] == '\0' || !password || password[0] == '\0') return;
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
    prefs.putString(passKey, password);
    prefs.end();
}

void saveApSettings(const char* ssid, bool secure, const char* password) {
    Preferences prefs;
    if (!prefs.begin("wifi_ap", false)) return;
    prefs.putString("ssid", ssid);
    prefs.putBool("secure", secure);
    prefs.putString("pass", secure ? password : "");
    prefs.end();
}

} // namespace

void init() {
    WiFi.mode(WIFI_STA);
    WiFi.disconnect(true);
}

void update() {
    switch (currentState) {
        case NetworkState::Scanning: {
            int n = WiFi.scanComplete();
            if (n != WIFI_SCAN_RUNNING) {
                scanCount = (n < 0) ? 0 : n;
                currentState = NetworkState::Disconnected;
            }
            break;
        }
        case NetworkState::ConnectingSta: {
            if (WiFi.status() == WL_CONNECTED) {
                currentState = NetworkState::ConnectedStaLocal;

                if (lastPassword[0] != '\0') {
                    saveNetworkCredential(targetSsid, lastPassword);
                }

                if (!ntpConfigured) {
                    configTzTime(kTimezonePosix, kNtpServer1, kNtpServer2);
                    ntpConfigured = true;
                }
            } else if (millis() - connectStartTime > kConnectTimeoutMs) {
                WiFi.disconnect();
                currentState = NetworkState::Disconnected;
            }
            break;
        }
        case NetworkState::ConnectedStaLocal: {
            if (WiFi.status() != WL_CONNECTED) {
                currentState = NetworkState::Disconnected;
                break;
            }
            // Execute DNS probe to verify external route
            IPAddress ip;
            if (WiFi.hostByName("pool.ntp.org", ip)) {
                currentState = NetworkState::ConnectedStaInternet;
            }
            break;
        }
        case NetworkState::ConnectedStaInternet: {
            if (WiFi.status() != WL_CONNECTED) {
                currentState = NetworkState::Disconnected;
            }
            break;
        }
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
    currentState = NetworkState::ConnectingSta;
}

void startAp(const char* ssid, const char* password, bool secure) {
    WiFi.mode(WIFI_AP_STA); // Forces hardware to support both
    if (WiFi.softAP(ssid, secure ? password : nullptr)) {
        apRunning = true;
        saveApSettings(ssid, secure, password);
    }
}

void stopAp() {
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA); // Revert to STA-only to save power/cycles
    apRunning = false;
}

bool isApEnabled() {
    return apRunning;
}

void scanNetworks() {
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
    ssidOut[0] = '\0'; passOut[0] = '\0'; secureOut = true;
    Preferences prefs;
    if (!prefs.begin("wifi_ap", true)) return;
    strncpy(ssidOut, prefs.getString("ssid", "").c_str(), ssidLen - 1);
    secureOut = prefs.getBool("secure", true);
    strncpy(passOut, prefs.getString("pass", "").c_str(), passLen - 1);
    prefs.end();
}

} // namespace network_manager
