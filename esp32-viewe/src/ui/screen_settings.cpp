#include "screen_settings.h"

#include "screen_debug.h"
#include "screen_info.h"
#include "screen_setup.h"
#include "screen_wifi.h"

namespace screen_settings {

lv_obj_t* create(lv_obj_t* parent) {
    lv_obj_t* tabview = lv_tabview_create(parent, LV_DIR_TOP, 40);

    lv_obj_t* wifiTab = lv_tabview_add_tab(tabview, "Wi-Fi");
    screen_wifi::create(wifiTab);

    lv_obj_t* setupTab = lv_tabview_add_tab(tabview, "Setup");
    screen_setup::create(setupTab);

    lv_obj_t* infoTab = lv_tabview_add_tab(tabview, "Info");
    screen_info::create(infoTab);

    lv_obj_t* debugTab = lv_tabview_add_tab(tabview, "Debug");
    screen_debug::create(debugTab);

    return tabview;
}

} // namespace screen_settings
