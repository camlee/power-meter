#pragma once

#include <stdint.h>

// A small, thread-safe virtual pointer source. HTTP handlers write here and
// the LVGL input callback consumes it, so no network task ever calls LVGL.
namespace remote_input {

void tap(uint16_t x, uint16_t y);
void setPointer(uint16_t x, uint16_t y, bool pressed);
bool read(uint16_t& x, uint16_t& y, bool& pressed);

} // namespace remote_input
