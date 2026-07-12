#include "screen_manager.h"
#include "ui_theme.h"

void ScreenManager::init() {
    ui_theme::init();
    // Create a single persistent root screen
    root_scr = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(root_scr, ui_theme::background(), 0);
    lv_obj_set_style_bg_opa(root_scr, LV_OPA_COVER, 0);
    // lv_obj_set_style_bg_color(root_scr, lv_color_black(), 0);

    // Remove default padding
    lv_obj_set_style_pad_all(root_scr, 0, 0);
    lv_obj_set_style_border_width(root_scr, 0, 0);

    // Create Tabview
    // Parameters: parent, tab direction (e.g., top, bottom, left, right), header size
    tabview = lv_tabview_create(root_scr, LV_DIR_TOP, 40);
    lv_obj_set_style_bg_color(tabview, ui_theme::background(), 0);
    lv_obj_set_style_bg_opa(tabview, LV_OPA_COVER, 0);
    // lv_obj_set_style_bg_color(tabview, lv_color_black(), 0);

    // Optional: Style the tab buttons background (the header area)
    // lv_obj_t* tab_btns = lv_tabview_get_tab_btns(tabview);
    // lv_obj_set_style_bg_color(tab_btns, lv_palette_darken(LV_PALETTE_GREY, 4), 0);

    lv_scr_load(root_scr);
}

void ScreenManager::registerScreen(const char* name, ScreenCreateFunc createFunc) {
    // We no longer need dot and tile pointers initialized, just the tab pointer
    screens.push_back({name, createFunc, nullptr});
}

void ScreenManager::build() {
    for (size_t i = 0; i < screens.size(); i++) {
        // Create the tab page natively using the registered name
        screens[i].tab = lv_tabview_add_tab(tabview, screens[i].name);

        // Remove default padding from the generated tab page
        // so the user UI function has full control of the space
        lv_obj_set_style_pad_all(screens[i].tab, 0, 0);
        lv_obj_set_style_border_width(screens[i].tab, 0, 0);
        // lv_obj_set_style_bg_color(screens[i].tab, lv_color_black(), 0);

        // Call the user's UI function, passing the new tab page as the parent
        screens[i].createFunc(screens[i].tab);
    }
}
