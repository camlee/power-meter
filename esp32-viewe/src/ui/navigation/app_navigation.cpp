#include "app_navigation.h"

#include <lvgl.h>

#include "screen_manager.h"
#include "settings_screen.h"
#include "../screens/home_screen.h"
#include "../screens/power_screen.h"
#include "../screens/sensors_screen.h"
#include "../screens/usage_screen.h"

namespace ui_navigation {
namespace {

struct TopLevelScreenDef {
    const char* title;
    ScreenCreateFunc create;
};

constexpr TopLevelScreenDef kTopLevelScreens[] = {
    {LV_SYMBOL_HOME, home_screen::create},
    {"Usage", usage_screen::create},
    {"Power", power_screen::create},
    {"Sensors", sensors_screen::create},
    {LV_SYMBOL_SETTINGS, settings_screen::create},
};

} // namespace

void build() {
    ScreenManager& screens = ScreenManager::instance();
    screens.init();

    for (const TopLevelScreenDef& screen : kTopLevelScreens) {
        screens.registerScreen(screen.title, screen.create);
    }

    screens.build();
}

void showSensors() {
    ScreenManager::instance().showScreen(3);
}

} // namespace ui_navigation
