#pragma once

#include <cstdint>

namespace status_display {

enum class Mode : uint8_t { Summary, Dense };

bool begin();
void update();
bool isReady();
Mode mode();
const char* modeName();
bool setMode(Mode mode);

} // namespace status_display
