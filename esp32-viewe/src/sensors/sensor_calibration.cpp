#include "sensor_calibration.h"

#include "sensor_config.h"
#include "sensors.h"

#include <Preferences.h>
#include <cmath>
#include <cstring>
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>

namespace sensors::calibration {
namespace {

constexpr uint32_t kMagic = 0x43414C31; // "CAL1"
constexpr uint16_t kVersion = 1;
constexpr char kKey[] = "cal_v1";

struct Profile {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    Value values[SENSOR_COUNT][2];
};

Profile profile{};
bool loaded = false;
portMUX_TYPE profileMux = portMUX_INITIALIZER_UNLOCKED;

Value defaultValue(Measurement measurement) {
    return measurement == Measurement::Voltage
        ? Value{config::kVoltageVoltsPerInputVolt, config::kVoltageOffsetV}
        : Value{config::kCurrentAmpsPerInputVolt, config::kCurrentOffsetV};
}

void resetProfile(Profile& value) {
    value = {};
    value.magic = kMagic;
    value.version = kVersion;
    for (uint8_t sensor = 0; sensor < SENSOR_COUNT; ++sensor) {
        value.values[sensor][static_cast<uint8_t>(Measurement::Voltage)] = defaultValue(Measurement::Voltage);
        value.values[sensor][static_cast<uint8_t>(Measurement::Current)] = defaultValue(Measurement::Current);
    }
}

bool validProfile(const Profile& value) {
    if (value.magic != kMagic || value.version != kVersion) return false;
    for (uint8_t sensor = 0; sensor < SENSOR_COUNT; ++sensor) {
        if (!isValid(Measurement::Voltage, value.values[sensor][0]) ||
            !isValid(Measurement::Current, value.values[sensor][1])) return false;
    }
    return true;
}

void loadIfNeeded() {
    portENTER_CRITICAL(&profileMux);
    const bool alreadyLoaded = loaded;
    portEXIT_CRITICAL(&profileMux);
    if (alreadyLoaded) return;

    Profile loadedProfile{};
    Preferences prefs;
    bool ok = prefs.begin("sensor_cal", true) && prefs.getBytesLength(kKey) == sizeof(loadedProfile) &&
              prefs.getBytes(kKey, &loadedProfile, sizeof(loadedProfile)) == sizeof(loadedProfile);
    prefs.end();
    if (!ok || !validProfile(loadedProfile)) resetProfile(loadedProfile);

    portENTER_CRITICAL(&profileMux);
    if (!loaded) {
        profile = loadedProfile;
        loaded = true;
    }
    portEXIT_CRITICAL(&profileMux);
}

} // namespace

void init() { loadIfNeeded(); }

Value defaults(Measurement measurement) { return defaultValue(measurement); }

bool isValid(Measurement measurement, Value value) {
    if (!std::isfinite(value.gain) || !std::isfinite(value.offsetInputV)) return false;
    // Broad coefficient bounds catch corrupt NVS/input while allowing normal
    // component tolerance around the present divider and current sensor.
    if (value.gain <= 0.0f || value.gain > 100.0f) return false;
    if (value.offsetInputV < kAdcMinInputV || value.offsetInputV > kAdcMaxInputV) return false;
    (void)measurement;
    return true;
}

Value get(uint8_t sensor, Measurement measurement) {
    loadIfNeeded();
    if (sensor >= SENSOR_COUNT) return defaultValue(measurement);
    portENTER_CRITICAL(&profileMux);
    Value value = profile.values[sensor][static_cast<uint8_t>(measurement)];
    portEXIT_CRITICAL(&profileMux);
    return value;
}

float apply(float inputV, Value value) { return (inputV - value.offsetInputV) * value.gain; }

bool set(uint8_t sensor, Measurement measurement, Value value) {
    loadIfNeeded();
    if (sensor >= SENSOR_COUNT || !isValid(measurement, value)) return false;

    Profile candidate;
    portENTER_CRITICAL(&profileMux);
    candidate = profile;
    candidate.values[sensor][static_cast<uint8_t>(measurement)] = value;
    portEXIT_CRITICAL(&profileMux);

    Preferences prefs;
    if (!prefs.begin("sensor_cal", false)) return false;
    const bool wrote = prefs.putBytes(kKey, &candidate, sizeof(candidate)) == sizeof(candidate);
    Profile readback{};
    const bool read = wrote && prefs.getBytes(kKey, &readback, sizeof(readback)) == sizeof(readback);
    prefs.end();
    if (!read || std::memcmp(&candidate, &readback, sizeof(candidate)) != 0) return false;

    portENTER_CRITICAL(&profileMux);
    profile = candidate;
    portEXIT_CRITICAL(&profileMux);
    return true;
}

} // namespace sensors::calibration
