#include "settings_screen.h"
#include "screen_manager.h"

#include "../screens/settings/device_info_screen.h"
#include "../screens/settings/device_setup_screen.h"
#include "../screens/settings/diagnostics_screen.h"
#include "../screens/settings/history_screen.h"
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
    {"Data", history_screen::create},
    {"Info", device_info_screen::create},
    {"Debug", diagnostics_screen::create},
};

struct SettingsTabRuntime {
    lv_obj_t* content = nullptr;
};

void notifyActiveTab(lv_obj_t* tabview, SettingsTabRuntime* tabs) {
    if (!tabview || !tabs) return;
    const uint16_t active = lv_tabview_get_tab_act(tabview);
    if (active < sizeof(kSettingsTabs) / sizeof(kSettingsTabs[0]) && tabs[active].content) {
        lv_event_send(tabs[active].content, LV_EVENT_REFRESH, nullptr);
    }
}

void tabChangedCb(lv_event_t* event) {
    notifyActiveTab(lv_event_get_target(event),
                    static_cast<SettingsTabRuntime*>(lv_event_get_user_data(event)));
}

void screenRefreshCb(lv_event_t* event) {
    if (lv_event_get_code(event) != LV_EVENT_REFRESH) return;
    notifyActiveTab(lv_event_get_target(event),
                    static_cast<SettingsTabRuntime*>(lv_event_get_user_data(event)));
}

} // namespace

lv_obj_t* create(lv_obj_t* parent) {
    lv_obj_t* tabview = lv_tabview_create(parent, LV_DIR_TOP, 40);
    static SettingsTabRuntime tabs[sizeof(kSettingsTabs) / sizeof(kSettingsTabs[0])];
    lv_obj_set_style_bg_color(tabview, ui_theme::background(), 0);
    lv_obj_set_style_bg_opa(tabview, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(tabview, 0, 0);
    lv_obj_set_style_radius(tabview, 0, 0);
    lv_obj_set_style_shadow_width(tabview, 0, 0);
    lv_obj_set_style_pad_all(tabview, 0, 0);

    for (size_t i = 0; i < sizeof(kSettingsTabs) / sizeof(kSettingsTabs[0]); ++i) {
        const SettingsTabDef& tabDef = kSettingsTabs[i];
        lv_obj_t* tab = lv_tabview_add_tab(tabview, tabDef.title);
        prepareTab(tab);
        tabs[i].content = tabDef.create(tab);
    }
    lv_obj_add_event_cb(tabview, tabChangedCb, LV_EVENT_VALUE_CHANGED, tabs);
    lv_obj_add_event_cb(tabview, screenRefreshCb, LV_EVENT_REFRESH, tabs);
    notifyActiveTab(tabview, tabs);

    return tabview;
}

} // namespace settings_screen
