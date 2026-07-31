#pragma once

#include "sensor_mode.h"
#include "sensors.h"

#include <cstdint>

namespace sensors::mapping {

constexpr uint8_t kPhysicalSensorCount = 3;

enum class PhysicalSensorId : uint8_t {
    Sensor1 = 0,
    Sensor2 = 1,
    Sensor3 = 2,
};

enum class LogicalRole : uint8_t {
    Unmapped = 0,
    Solar = 1,
    Load = 2,
    Battery = 3,
};

enum class CurrentDirection : int8_t {
    Normal = 1,
    Reversed = -1,
};

struct Entry {
    LogicalRole role = LogicalRole::Unmapped;
    CurrentDirection currentDirection = CurrentDirection::Normal;
};

struct Profile {
    Entry physical[kPhysicalSensorCount]{};
};

// Mapping is stored per acquisition source. ADC defaults preserve phase (c):
// Sensor 3 is physically reversed so Battery charging is positive. Sources
// that already report the logical convention retain their existing direction.
constexpr Profile defaults(sensor_mode::Mode mode) {
    Profile profile{{
        {LogicalRole::Solar, CurrentDirection::Normal},
        {LogicalRole::Load, CurrentDirection::Normal},
        {LogicalRole::Battery,
         mode == sensor_mode::Mode::Adc
             ? CurrentDirection::Reversed : CurrentDirection::Normal},
    }};
    return profile;
}

constexpr bool isValidRole(LogicalRole role) {
    return role == LogicalRole::Unmapped || role == LogicalRole::Solar ||
           role == LogicalRole::Load || role == LogicalRole::Battery;
}

constexpr bool isValidDirection(CurrentDirection direction) {
    return direction == CurrentDirection::Normal ||
           direction == CurrentDirection::Reversed;
}

constexpr bool isValid(const Profile& profile) {
    uint8_t solarCount = 0;
    uint8_t loadCount = 0;
    uint8_t batteryCount = 0;
    for (const Entry& entry : profile.physical) {
        if (!isValidRole(entry.role) || !isValidDirection(entry.currentDirection)) {
            return false;
        }
        if (entry.role == LogicalRole::Solar) ++solarCount;
        else if (entry.role == LogicalRole::Load) ++loadCount;
        else if (entry.role == LogicalRole::Battery) ++batteryCount;
    }
    return solarCount == 1 && loadCount == 1 && batteryCount <= 1;
}

constexpr LogicalRole roleForLogical(SensorId logical) {
    switch (logical) {
        case SENSOR_SOLAR: return LogicalRole::Solar;
        case SENSOR_LOAD: return LogicalRole::Load;
        case SENSOR_BATTERY: return LogicalRole::Battery;
        default: return LogicalRole::Unmapped;
    }
}

constexpr bool logicalForRole(LogicalRole role, SensorId& logical) {
    switch (role) {
        case LogicalRole::Solar: logical = SENSOR_SOLAR; return true;
        case LogicalRole::Load: logical = SENSOR_LOAD; return true;
        case LogicalRole::Battery: logical = SENSOR_BATTERY; return true;
        case LogicalRole::Unmapped: return false;
    }
    return false;
}

constexpr int8_t multiplier(CurrentDirection direction) {
    return static_cast<int8_t>(direction);
}

constexpr bool physicalForLogical(const Profile& profile, SensorId logical,
                                  PhysicalSensorId& physical) {
    const LogicalRole role = roleForLogical(logical);
    if (role == LogicalRole::Unmapped) return false;
    for (uint8_t index = 0; index < kPhysicalSensorCount; ++index) {
        if (profile.physical[index].role == role) {
            physical = static_cast<PhysicalSensorId>(index);
            return true;
        }
    }
    return false;
}

constexpr bool logicalForPhysical(const Profile& profile,
                                  PhysicalSensorId physical,
                                  SensorId& logical) {
    const uint8_t index = static_cast<uint8_t>(physical);
    return index < kPhysicalSensorCount &&
           logicalForRole(profile.physical[index].role, logical);
}

constexpr int8_t currentMultiplier(const Profile& profile,
                                   PhysicalSensorId physical) {
    const uint8_t index = static_cast<uint8_t>(physical);
    return index < kPhysicalSensorCount
        ? multiplier(profile.physical[index].currentDirection) : 1;
}

void init();
Profile get(sensor_mode::Mode mode);
bool set(sensor_mode::Mode mode, const Profile& profile);
bool set(sensor_mode::Mode mode, const Profile& profile, bool balanceVisible);
bool set(sensor_mode::Mode mode, const Profile& profile, bool balanceVisible,
         bool calibrationControlsVisible);
bool balanceVisible();
bool calibrationControlsVisible();

bool physicalForLogical(sensor_mode::Mode mode, SensorId logical,
                        PhysicalSensorId& physical);
bool logicalForPhysical(sensor_mode::Mode mode, PhysicalSensorId physical,
                        SensorId& logical);
int8_t currentMultiplier(sensor_mode::Mode mode, PhysicalSensorId physical);
int8_t currentMultiplierForLogical(sensor_mode::Mode mode, SensorId logical);

const char* physicalId(PhysicalSensorId physical);
const char* physicalLabel(PhysicalSensorId physical);
const char* roleName(LogicalRole role);
const char* directionName(CurrentDirection direction);
bool parseRole(const char* value, LogicalRole& role);
bool parseDirection(const char* value, CurrentDirection& direction);

} // namespace sensors::mapping
