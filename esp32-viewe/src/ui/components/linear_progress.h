#pragma once

#include <lvgl.h>

namespace linear_progress {

// A reusable, thin indeterminate loading indicator. It is floating, so it
// does not affect a screen's layout while history work runs in the background.
lv_obj_t* create(lv_obj_t* parent);
void show(lv_obj_t* progress);
void hide(lv_obj_t* progress);

} // namespace linear_progress
