#pragma once

namespace sensor_mode {
enum class Mode : unsigned char { Real = 0, Demo = 1 };
Mode get();
bool set(Mode mode);
const char* label();
} // namespace sensor_mode
