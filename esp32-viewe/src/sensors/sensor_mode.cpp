#include "sensor_mode.h"
#include <Preferences.h>
#include "sensor_config.h"

namespace sensor_mode {
namespace {
bool loaded = false;
Mode current;
void load() {
    if (loaded) return;
    Preferences prefs;
    const bool defaultDemo = POWER_METER_USE_SIMULATED_SENSORS;
    current = prefs.begin("sensors", true) && prefs.getBool("demo", defaultDemo) ? Mode::Demo : Mode::Real;
    prefs.end();
    loaded = true;
}
}
Mode get() { load(); return current; }
bool set(Mode mode) {
    Preferences prefs;
    if (!prefs.begin("sensors", false)) return false;
    const bool ok = prefs.putBool("demo", mode == Mode::Demo) == 1;
    prefs.end(); current = mode; loaded = true; return ok;
}
const char* label() { return get() == Mode::Demo ? "Demo" : "Real"; }
} // namespace sensor_mode
