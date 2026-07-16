#pragma once

#include <stdint.h>
#include <stddef.h>
#include "network_policy.h"

namespace network_manager {

enum class NetworkState {
    Disconnected,
    ConnectingSta,
    ConnectedStaLocal,
    ConnectedStaInternet
};

enum class ConnectionPhase {
    Idle,
    LookingForNetwork,
    ObtainingIp,
    RetryWaiting,
    ActionRequired,
};

enum class ScanState {
    Idle,
    Starting,
    Running,
    Succeeded,
    Failed,
};

// Subsystem initialization and non-blocking event loop tick
void init();
void update();

// Command API
bool connectTo(const char* ssid, const char* password = nullptr);
void disconnect();
void startAp(const char* ssid, const char* password = nullptr, bool secure = true);
void stopAp();
bool scanNetworks();

// State Querying
NetworkState getState();
ConnectionPhase getConnectionPhase();
ConnectionFailure getConnectionFailure();
const char* getCurrentSsid();
uint32_t getReconnectSecondsRemaining();
int getRssi();
bool isApEnabled();
const char* getStaIpAddress();
const char* getApIpAddress();
const char* getHostname();
// Re-advertise the current device identity after a device ID change.
void restartMdns();

// Scan Results API
ScanState getScanState();
uint32_t getScanGeneration();
int getScanResultCount();
bool getScanResult(int index, char* ssidOut, size_t ssidLen, bool& secureOut, int& rssiOut);

// Access Point Querying
int getApClientCount();
bool getApClientMac(int index, char* macStrOut, size_t maxLen);

// Persistence (NVS) Queries for the UI
int getSavedNetworkCount();
bool getSavedNetwork(int index, char* ssidOut, size_t ssidLen);
bool connectSavedNetwork(const char* ssid);
bool forgetSavedNetwork(const char* ssid);
bool getSavedPassword(const char* ssid, char* passOut, size_t maxLen);
void getSavedApSettings(char* ssidOut, size_t ssidLen, bool& secureOut, char* passOut, size_t passLen);
// Removes both station and access-point credentials from NVS.
bool clearSavedCredentials();

} // namespace network_manager
