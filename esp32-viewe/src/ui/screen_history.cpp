#include "screen_history.h"

#include <lvgl.h>

namespace ui::screen_history {

void create()
{
    lv_obj_clean(lv_scr_act());
    lv_obj_t *label = lv_label_create(lv_scr_act());
    lv_label_set_text(label, "History");
    lv_obj_center(label);
}

} // namespace ui::screen_history

