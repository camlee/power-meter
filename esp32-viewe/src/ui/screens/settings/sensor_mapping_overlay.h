#pragma once

namespace sensor_mapping_overlay {

// Opens the full-screen editor for the currently active acquisition source.
// The overlay retains a draft until Save or Cancel; saving persists the
// complete source profile and restarts acquisition.
void show();

} // namespace sensor_mapping_overlay
