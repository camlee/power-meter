#pragma once

#include "sensors/sensor_calibration.h"
#include "sensors/sensor_mapping.h"

namespace sensor_calibration_overlay {

// Opens the one shared full-screen calibration workflow for a physical
// Sensor 1/2/3 channel. Calibration remains attached to the physical sensor.
void show(
    sensors::mapping::PhysicalSensorId physical,
    sensors::calibration::Measurement measurement);

// Allows a mapping editor to display its draft role while current
// interpretation continues to use the persisted direction.
void show(
    sensors::mapping::PhysicalSensorId physical,
    sensors::calibration::Measurement measurement,
    sensors::mapping::LogicalRole displayRole);

} // namespace sensor_calibration_overlay
