#pragma once
#include "lvgl.h"

// Derived/combined power metrics rather than raw per-sensor readings.

namespace power_screen {
lv_obj_t* create(lv_obj_t* parent);
} // namespace power_screen
