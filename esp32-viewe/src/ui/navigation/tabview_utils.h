#pragma once

#include <lvgl.h>

namespace tabview_utils {

// LVGL tabviews implement swipe navigation by making their content container
// scrollable. Programmatic scrolling used by tab-button presses still works
// when this input flag is cleared.
inline void disableSwipe(lv_obj_t* tabview) {
    lv_obj_t* content = lv_tabview_get_content(tabview);
    lv_obj_clear_flag(content, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_CHAIN);
}

} // namespace tabview_utils
