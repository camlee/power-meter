#include "settings_screen.h"
#include "screen_manager.h"

#include "../screens/settings/device_info_screen.h"
#include "../screens/settings/device_setup_screen.h"
#include "../screens/settings/diagnostics_screen.h"
#include "../screens/settings/wifi_screen.h"
#include "../theme/ui_theme.h"

namespace settings_screen {
namespace {

void prepareTab(lv_obj_t* tab) {
    // Tabview pages come with default inset padding. Each child screen owns
    // its own spacing, so leaving it in place creates an unnecessary second
    // gutter around every Settings page.
    lv_obj_set_style_pad_all(tab, 0, 0);
    lv_obj_set_style_border_width(tab, 0, 0);
}

struct SettingsTabDef {
    const char* title;
    ScreenCreateFunc create;
};

constexpr SettingsTabDef kSettingsTabs[] = {
    {"Wi-Fi", wifi_screen::create},
    {"Setup", device_setup_screen::create},
    {"Info", device_info_screen::create},
    {"Debug", diagnostics_screen::create},
};

} // namespace

lv_obj_t* create(lv_obj_t* parent) {
    lv_obj_t* tabview = lv_tabview_create(parent, LV_DIR_TOP, 40);
    lv_obj_set_style_bg_color(tabview, ui_theme::background(), 0);
    lv_obj_set_style_bg_opa(tabview, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(tabview, 0, 0);
    lv_obj_set_style_radius(tabview, 0, 0);
    lv_obj_set_style_shadow_width(tabview, 0, 0);
    lv_obj_set_style_pad_all(tabview, 0, 0);

    for (const SettingsTabDef& tabDef : kSettingsTabs) {
        lv_obj_t* tab = lv_tabview_add_tab(tabview, tabDef.title);
        prepareTab(tab);
        tabDef.create(tab);
    }

    return tabview;
}

} // namespace settings_screen
