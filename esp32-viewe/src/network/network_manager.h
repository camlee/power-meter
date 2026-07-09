#pragma once

#include <stdint.h>
#include <stddef.h>

namespace network_manager {

enum class NetworkState {
    Disconnected,
    Scanning,
    ConnectingSta,
    ConnectedStaLocal,
    ConnectedStaInternet
};

// Subsystem initialization and non-blocking event loop tick
void init();
void update();

// Command API
void connectTo(const char* ssid, const char* password = nullptr);
void disconnect();
void startAp(const char* ssid, const char* password = nullptr, bool secure = true);
void stopAp();
void scanNetworks();

// State Querying
NetworkState getState();
const char* getCurrentSsid();
int getRssi();
bool isApEnabled();

// Scan Results API
int getScanResultCount();
bool getScanResult(int index, char* ssidOut, size_t ssidLen, bool& secureOut, int& rssiOut);

// Access Point Querying
int getApClientCount();
bool getApClientMac(int index, char* macStrOut, size_t maxLen);

// Persistence (NVS) Queries for the UI
bool getSavedPassword(const char* ssid, char* passOut, size_t maxLen);
void getSavedApSettings(char* ssidOut, size_t ssidLen, bool& secureOut, char* passOut, size_t passLen);

} // namespace network_manager
