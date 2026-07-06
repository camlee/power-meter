#pragma once
#include <lvgl.h>
#include "screen_manager.h"

// Adds a row of navigation buttons across the top of `parent`, one per
// registered screen. The button for `current` is disabled so you can't
// "navigate" to the screen you're already on.
namespace nav_bar {
void create(lv_obj_t* parent, ScreenId current);
}
