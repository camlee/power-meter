#include "device_identity.h"

#include <Arduino.h>
#include <Preferences.h>
#include <esp_random.h>

#include <ctype.h>
#include <string.h>

namespace device_identity {
namespace {

constexpr size_t kDeviceIdMaxLen = 31;  // DNS hostname limit is 63; keep UI-friendly.
char deviceId[kDeviceIdMaxLen + 1] = {0};
char hardwareId[18] = {0};  // 12 hex MAC chars plus NUL.

bool isValidDeviceId(const char* value) {
    if (!value || value[0] == '\0') return false;
    size_t len = strlen(value);
    if (len > kDeviceIdMaxLen || value[0] == '-' || value[len - 1] == '-') return false;
    for (size_t i = 0; i < len; ++i) {
        if (!(value[i] == '-' || (value[i] >= 'a' && value[i] <= 'z') ||
              (value[i] >= '0' && value[i] <= '9'))) {
            return false;
        }
    }
    return true;
}

void generateDeviceId() {
    // Base32-like alphabet excludes visually ambiguous characters.  Eight
    // random characters give more than one trillion possible default names.
    constexpr char kAlphabet[] = "abcdefghjkmnpqrstuvwxyz23456789";
    constexpr size_t kSuffixLength = 8;
    snprintf(deviceId, sizeof(deviceId), "meter-");
    size_t prefix = strlen(deviceId);
    for (size_t i = 0; i < kSuffixLength; ++i) {
        deviceId[prefix + i] = kAlphabet[esp_random() % (sizeof(kAlphabet) - 1)];
    }
    deviceId[prefix + kSuffixLength] = '\0';
}

} // namespace

void init() {
    uint64_t mac = ESP.getEfuseMac();
    snprintf(hardwareId, sizeof(hardwareId), "%012llx", static_cast<unsigned long long>(mac & 0xFFFFFFFFFFFFULL));

    Preferences prefs;
    if (!prefs.begin("device", false)) {
        generateDeviceId();
        return;
    }
    String persistedId = prefs.getString("id", "");
    if (isValidDeviceId(persistedId.c_str())) {
        strncpy(deviceId, persistedId.c_str(), sizeof(deviceId) - 1);
        deviceId[sizeof(deviceId) - 1] = '\0';
    } else {
        generateDeviceId();
        prefs.putString("id", deviceId);
    }
    prefs.end();
}

const char* getDeviceId() { return deviceId; }
const char* getHostname() { return deviceId; }
const char* getHardwareId() { return hardwareId; }

bool setDeviceId(const char* newDeviceId) {
    if (!isValidDeviceId(newDeviceId)) return false;
    Preferences prefs;
    if (!prefs.begin("device", false)) return false;
    bool saved = prefs.putString("id", newDeviceId) > 0;
    prefs.end();
    if (!saved) return false;
    strncpy(deviceId, newDeviceId, sizeof(deviceId) - 1);
    deviceId[sizeof(deviceId) - 1] = '\0';
    return true;
}

} // namespace device_identity
