#include "sensor_mapping.h"

#include "device/device_state.h"

#include <Preferences.h>
#include <cstring>
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>

namespace sensors::mapping {
namespace {

constexpr char kPreferencesNamespace[] = "sensor_map";
constexpr char kProfileKey[] = "map_v1";
constexpr uint32_t kMagic = 0x4D415031; // "MAP1"
constexpr uint16_t kVersion = 1;
constexpr uint8_t kModeCount = 4;

struct StoredProfiles {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    Profile profiles[kModeCount];
};

StoredProfiles stored{};
bool loaded = false;
portMUX_TYPE mappingMux = portMUX_INITIALIZER_UNLOCKED;

constexpr uint8_t modeIndex(sensor_mode::Mode mode) {
    return static_cast<uint8_t>(mode);
}

bool validStored(const StoredProfiles& value) {
    if (value.magic != kMagic || value.version != kVersion) return false;
    for (uint8_t mode = 0; mode < kModeCount; ++mode) {
        if (!isValid(value.profiles[mode])) return false;
    }
    return true;
}

void reset(StoredProfiles& value) {
    value = {};
    value.magic = kMagic;
    value.version = kVersion;
    for (uint8_t mode = 0; mode < kModeCount; ++mode) {
        value.profiles[mode] = defaults(static_cast<sensor_mode::Mode>(mode));
    }
}

void loadIfNeeded() {
    portENTER_CRITICAL(&mappingMux);
    const bool alreadyLoaded = loaded;
    portEXIT_CRITICAL(&mappingMux);
    if (alreadyLoaded) return;

    StoredProfiles candidate{};
    reset(candidate);
    Preferences preferences;
    const bool opened = preferences.begin(kPreferencesNamespace, true);
    const bool valid =
        opened && preferences.getBytesLength(kProfileKey) == sizeof(candidate) &&
        preferences.getBytes(kProfileKey, &candidate, sizeof(candidate)) ==
            sizeof(candidate) &&
        validStored(candidate);
    if (opened) preferences.end();
    if (!valid) reset(candidate);

    portENTER_CRITICAL(&mappingMux);
    if (!loaded) {
        stored = candidate;
        loaded = true;
    }
    portEXIT_CRITICAL(&mappingMux);
}

} // namespace

void init() { loadIfNeeded(); }

Profile get(sensor_mode::Mode mode) {
    loadIfNeeded();
    const uint8_t index = modeIndex(mode);
    if (index >= kModeCount) return defaults(mode);
    portENTER_CRITICAL(&mappingMux);
    const Profile result = stored.profiles[index];
    portEXIT_CRITICAL(&mappingMux);
    return result;
}

bool set(sensor_mode::Mode mode, const Profile& profile) {
    loadIfNeeded();
    const uint8_t index = modeIndex(mode);
    if (index >= kModeCount || !isValid(profile)) return false;

    StoredProfiles candidate;
    portENTER_CRITICAL(&mappingMux);
    candidate = stored;
    portEXIT_CRITICAL(&mappingMux);
    candidate.profiles[index] = profile;

    Preferences preferences;
    if (!preferences.begin(kPreferencesNamespace, false)) return false;
    const bool wrote =
        preferences.putBytes(kProfileKey, &candidate, sizeof(candidate)) ==
        sizeof(candidate);
    StoredProfiles readback{};
    const bool read =
        wrote &&
        preferences.getBytes(kProfileKey, &readback, sizeof(readback)) ==
            sizeof(readback);
    preferences.end();
    if (!read || std::memcmp(&candidate, &readback, sizeof(candidate)) != 0 ||
        !validStored(readback)) {
        return false;
    }

    portENTER_CRITICAL(&mappingMux);
    stored = candidate;
    portEXIT_CRITICAL(&mappingMux);
    device_state::changed(device_state::Domain::SensorMapping);
    return true;
}

bool physicalForLogical(sensor_mode::Mode mode, SensorId logical,
                        PhysicalSensorId& physical) {
    return physicalForLogical(get(mode), logical, physical);
}

bool logicalForPhysical(sensor_mode::Mode mode, PhysicalSensorId physical,
                        SensorId& logical) {
    return logicalForPhysical(get(mode), physical, logical);
}

int8_t currentMultiplier(sensor_mode::Mode mode, PhysicalSensorId physical) {
    return currentMultiplier(get(mode), physical);
}

int8_t currentMultiplierForLogical(sensor_mode::Mode mode, SensorId logical) {
    const Profile profile = get(mode);
    PhysicalSensorId physical;
    return physicalForLogical(profile, logical, physical)
        ? currentMultiplier(profile, physical) : 1;
}

const char* physicalId(PhysicalSensorId physical) {
    switch (physical) {
        case PhysicalSensorId::Sensor1: return "sensor1";
        case PhysicalSensorId::Sensor2: return "sensor2";
        case PhysicalSensorId::Sensor3: return "sensor3";
    }
    return "unknown";
}

const char* physicalLabel(PhysicalSensorId physical) {
    switch (physical) {
        case PhysicalSensorId::Sensor1: return "Sensor 1";
        case PhysicalSensorId::Sensor2: return "Sensor 2";
        case PhysicalSensorId::Sensor3: return "Sensor 3";
    }
    return "Unknown";
}

const char* roleName(LogicalRole role) {
    switch (role) {
        case LogicalRole::Unmapped: return "unmapped";
        case LogicalRole::Solar: return "solar";
        case LogicalRole::Load: return "load";
        case LogicalRole::Battery: return "battery";
    }
    return "unmapped";
}

const char* directionName(CurrentDirection direction) {
    return direction == CurrentDirection::Reversed ? "reversed" : "normal";
}

bool parseRole(const char* value, LogicalRole& role) {
    if (!value) return false;
    if (std::strcmp(value, "unmapped") == 0) role = LogicalRole::Unmapped;
    else if (std::strcmp(value, "solar") == 0) role = LogicalRole::Solar;
    else if (std::strcmp(value, "load") == 0) role = LogicalRole::Load;
    else if (std::strcmp(value, "battery") == 0) role = LogicalRole::Battery;
    else return false;
    return true;
}

bool parseDirection(const char* value, CurrentDirection& direction) {
    if (!value) return false;
    if (std::strcmp(value, "normal") == 0) {
        direction = CurrentDirection::Normal;
    } else if (std::strcmp(value, "reversed") == 0) {
        direction = CurrentDirection::Reversed;
    } else {
        return false;
    }
    return true;
}

} // namespace sensors::mapping
