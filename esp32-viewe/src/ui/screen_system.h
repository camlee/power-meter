#pragma once
#include "lvgl.h"

// System-level view: derived/combined numbers rather than raw per-sensor
// readings. Pairs with screen_realtime.h, which shows the raw view.

namespace screen_system {
lv_obj_t* create(lv_obj_t* parent);
} // namespace screen_system
