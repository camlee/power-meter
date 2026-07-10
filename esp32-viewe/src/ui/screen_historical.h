#pragma once
#include <lvgl.h>

// Historical screen: long-term power/energy chart built from
// historical_storage's minute records. Refreshes on a slow timer since new
// data only lands once a minute -- no point polling faster than that.
namespace screen_historical {

lv_obj_t* create(lv_obj_t* parent);

} // namespace screen_historical
