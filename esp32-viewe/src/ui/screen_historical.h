#pragma once
#include <lvgl.h>

// Historical screen: a three-channel average-power chart built from minute
// records. Energy/session/time reconciliation will be added by the durable
// history milestone.
namespace screen_historical {

lv_obj_t* create(lv_obj_t* parent);

} // namespace screen_historical
