#include "sensor_mode.h"
#include <Preferences.h>
#include "device/hardware_profile.h"
#include "sensor_config.h"

namespace sensor_mode {
namespace {
constexpr char kPreferencesNamespace[] = "sensors";
constexpr char kModeV1Key[] = "mode_v1";

bool loaded = false;
Mode current = Mode::Adc;

bool isValid(Mode mode) {
    return mode == Mode::Adc || mode == Mode::Uart || mode == Mode::Demo;
}

bool supported(Mode mode) {
    switch (mode) {
        case Mode::Adc: return hardware_profile::kSupportsAdc;
        case Mode::Uart: return hardware_profile::kSupportsUart;
        case Mode::Demo: return hardware_profile::kSupportsDemo;
    }
    return false;
}

void load() {
    if (loaded) return;
    Preferences prefs;
    Mode defaultMode = POWER_METER_USE_SIMULATED_SENSORS ? Mode::Demo : Mode::Adc;
    if (!supported(defaultMode)) defaultMode = Mode::Demo;
    current = defaultMode;
    if (prefs.begin(kPreferencesNamespace, true)) {
        const Mode stored = static_cast<Mode>(prefs.getUChar(kModeV1Key, static_cast<uint8_t>(defaultMode)));
        if (isValid(stored) && supported(stored)) current = stored;
        prefs.end();
    }
    loaded = true;
}
}

Mode get() { load(); return current; }

bool isSupported(Mode mode) { return isValid(mode) && supported(mode); }

bool set(Mode mode) {
    if (!isSupported(mode)) return false;
    Preferences prefs;
    if (!prefs.begin(kPreferencesNamespace, false)) return false;
    const bool ok = prefs.putUChar(kModeV1Key, static_cast<uint8_t>(mode)) == 1;
    prefs.end();
    if (ok) {
        current = mode;
        loaded = true;
    }
    return ok;
}

const char* label() {
    switch (get()) {
        case Mode::Adc: return "ADC";
        case Mode::Uart: return "UART";
        case Mode::Demo: return "Demo";
    }
    return "ADC";
}

} // namespace sensor_mode
