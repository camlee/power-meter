#pragma once

#include "sensors/sensor_calibration.h"
#include "sensors/sensor_mapping.h"

namespace sensor_calibration_overlay {

// Opens the one shared full-screen calibration workflow for a physical
// Sensor 1/2/3 channel. The active persisted role/direction supplies the
// subtitle and current interpretation; calibration itself remains attached
// to the physical sensor.
void show(
    sensors::mapping::PhysicalSensorId physical,
    sensors::calibration::Measurement measurement);

} // namespace sensor_calibration_overlay
