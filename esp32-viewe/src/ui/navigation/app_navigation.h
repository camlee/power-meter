#pragma once

namespace ui_navigation {

// Creates the persistent top-level tab layout and registers all application
// screens in their display order.
void build();

// Opens the existing Sensors workflow. Sensor mapping uses this rather than
// maintaining a second calibration implementation.
void showSensors();

} // namespace ui_navigation
