#pragma once
#include <lvgl.h>

// Realtime screen: a tab per sensor (voltage + current charts) plus a
// "Combined Power" tab overlaying all 3 sensors' power. Only calls into
// sensors.h -- it has no idea whether the data is simulated or real.
namespace screen_realtime {

// Builds the screen as a child of `parent` and returns it, matching the
// create(parent) convention used elsewhere in the project (e.g. tiles
// managed by screen_manager). Call once during UI setup.
lv_obj_t* create(lv_obj_t* parent);

} // namespace screen_realtime
