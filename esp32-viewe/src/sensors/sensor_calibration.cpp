#include "sensor_calibration.h"

#include "sensor_config.h"
#include "sensor_mapping.h"
#include "device/device_state.h"

#include <Preferences.h>
#include <cmath>
#include <cstring>
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>

namespace sensors::calibration {
namespace {

constexpr uint32_t kMagic = 0x43414C32; // "CAL2"
constexpr uint16_t kVersion = 2;
constexpr char kKey[] = "cal_v2";
constexpr uint32_t kLegacyMagic = 0x43414C31; // "CAL1"
constexpr uint16_t kLegacyVersion = 1;
constexpr char kLegacyKey[] = "cal_v1";
constexpr uint8_t kSourceCount = static_cast<uint8_t>(Source::Count);

struct Profile {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    Value values[kSourceCount][mapping::kPhysicalSensorCount][2];
};

struct LegacyProfile {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    Value values[mapping::kPhysicalSensorCount][2];
};

Profile profile{};
bool loaded = false;
portMUX_TYPE profileMux = portMUX_INITIALIZER_UNLOCKED;

Value defaultValue(Source source, uint8_t sensor, Measurement measurement) {
    if (source == Source::Ads1115) {
        if (sensor == static_cast<uint8_t>(mapping::PhysicalSensorId::Sensor1)) {
            return measurement == Measurement::Voltage
                ? Value{21.3f, 0.0f} : Value{33.0f, 1.1613f};
        }
        if (sensor == static_cast<uint8_t>(mapping::PhysicalSensorId::Sensor2)) {
            return measurement == Measurement::Voltage
                ? Value{26.13f, 0.0f} : Value{36.0f, 0.957f};
        }
    }
    return measurement == Measurement::Voltage
        ? Value{config::kVoltageVoltsPerInputVolt, config::kVoltageOffsetV}
        : Value{config::kCurrentAmpsPerInputVolt, config::kCurrentOffsetV};
}

void resetProfile(Profile& value) {
    value = {};
    value.magic = kMagic;
    value.version = kVersion;
    for (uint8_t source = 0; source < kSourceCount; ++source) {
        for (uint8_t sensor = 0; sensor < mapping::kPhysicalSensorCount; ++sensor) {
            value.values[source][sensor][static_cast<uint8_t>(Measurement::Voltage)] =
                defaultValue(static_cast<Source>(source), sensor, Measurement::Voltage);
            value.values[source][sensor][static_cast<uint8_t>(Measurement::Current)] =
                defaultValue(static_cast<Source>(source), sensor, Measurement::Current);
        }
    }
}

bool validProfile(const Profile& value) {
    if (value.magic != kMagic || value.version != kVersion) return false;
    for (uint8_t source = 0; source < kSourceCount; ++source) {
        for (uint8_t sensor = 0; sensor < mapping::kPhysicalSensorCount; ++sensor) {
            if (!validate(
                     Measurement::Voltage, value.values[source][sensor][0],
                     ValidationPolicy::StoredProfile).accepted() ||
                !validate(
                     Measurement::Current, value.values[source][sensor][1],
                     ValidationPolicy::StoredProfile).accepted()) {
                return false;
            }
        }
    }
    return true;
}

bool validLegacyProfile(const LegacyProfile& value) {
    if (value.magic != kLegacyMagic || value.version != kLegacyVersion) return false;
    for (uint8_t sensor = 0; sensor < mapping::kPhysicalSensorCount; ++sensor) {
        if (!validate(
                 Measurement::Voltage, value.values[sensor][0],
                 ValidationPolicy::StoredProfile).accepted() ||
            !validate(
                 Measurement::Current, value.values[sensor][1],
                 ValidationPolicy::StoredProfile).accepted()) {
            return false;
        }
    }
    return true;
}

void loadIfNeeded() {
    portENTER_CRITICAL(&profileMux);
    const bool alreadyLoaded = loaded;
    portEXIT_CRITICAL(&profileMux);
    if (alreadyLoaded) return;

    Profile loadedProfile{};
    resetProfile(loadedProfile);
    Preferences prefs;
    bool opened = prefs.begin("sensor_cal", true);
    bool ok = opened && prefs.getBytesLength(kKey) == sizeof(loadedProfile) &&
              prefs.getBytes(kKey, &loadedProfile, sizeof(loadedProfile)) == sizeof(loadedProfile) &&
              validProfile(loadedProfile);
    if (!ok && opened && prefs.getBytesLength(kLegacyKey) == sizeof(LegacyProfile)) {
        LegacyProfile legacy{};
        if (prefs.getBytes(kLegacyKey, &legacy, sizeof(legacy)) == sizeof(legacy) &&
            validLegacyProfile(legacy)) {
            for (uint8_t sensor = 0; sensor < mapping::kPhysicalSensorCount; ++sensor) {
                loadedProfile.values[static_cast<uint8_t>(Source::Esp32Adc)][sensor][0] = legacy.values[sensor][0];
                loadedProfile.values[static_cast<uint8_t>(Source::Esp32Adc)][sensor][1] = legacy.values[sensor][1];
            }
            ok = true;
        }
    }
    prefs.end();
    if (!ok) resetProfile(loadedProfile);

    portENTER_CRITICAL(&profileMux);
    if (!loaded) {
        profile = loadedProfile;
        loaded = true;
    }
    portEXIT_CRITICAL(&profileMux);
}

} // namespace

void init() { loadIfNeeded(); }

Value defaults(Source source, uint8_t sensor, Measurement measurement) {
    return defaultValue(source, sensor, measurement);
}

Value get(Source source, uint8_t sensor, Measurement measurement) {
    loadIfNeeded();
    const uint8_t sourceIndex = static_cast<uint8_t>(source);
    if (sourceIndex >= kSourceCount || sensor >= mapping::kPhysicalSensorCount) {
        return defaultValue(source, sensor, measurement);
    }
    portENTER_CRITICAL(&profileMux);
    Value value = profile.values[sourceIndex][sensor][static_cast<uint8_t>(measurement)];
    portEXIT_CRITICAL(&profileMux);
    return value;
}

float apply(float inputV, Value value) { return (inputV - value.offsetInputV) * value.gain; }

bool set(Source source, uint8_t sensor, Measurement measurement, Value value) {
    loadIfNeeded();
    const uint8_t sourceIndex = static_cast<uint8_t>(source);
    if (sourceIndex >= kSourceCount ||
        sensor >= mapping::kPhysicalSensorCount ||
        !validate(
            measurement, value,
            ValidationPolicy::CommitCandidate).accepted()) {
        return false;
    }

    Profile candidate;
    portENTER_CRITICAL(&profileMux);
    candidate = profile;
    candidate.values[sourceIndex][sensor][static_cast<uint8_t>(measurement)] = value;
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
    device_state::changed(device_state::Domain::Calibration);
    return true;
}

} // namespace sensors::calibration
