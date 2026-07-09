#include "screen_test.h"
#include <Arduino.h>
#include <esp_system.h>

namespace screen_test {

lv_obj_t* create(lv_obj_t* parent) {
    lv_obj_t* scr = lv_obj_create(parent);

    // Strip default theme styles from the parent container
    lv_obj_remove_style_all(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0000FF), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

    lv_obj_set_size(scr, lv_pct(100), lv_pct(100));
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    // Explicitly disable scrollbar rendering to avoid visual artifacts
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);


    lv_obj_t * my_label1 = lv_label_create(scr);
    lv_label_set_text(my_label1, "Hello World !");

    return scr;
}

} // namespace screen_test
