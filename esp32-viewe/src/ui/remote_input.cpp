#include "remote_input.h"

#include <Arduino.h>

namespace remote_input {
namespace {

portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
uint16_t pointerX = 0;
uint16_t pointerY = 0;
bool pointerPressed = false;
uint32_t releaseAt = 0;

} // namespace

void tap(uint16_t x, uint16_t y) {
    portENTER_CRITICAL(&mux);
    pointerX = x;
    pointerY = y;
    pointerPressed = true;
    // LVGL must observe a pressed sample before the release sample. 75 ms is
    // long enough for the normal LVGL polling cadence without feeling slow.
    releaseAt = millis() + 75;
    portEXIT_CRITICAL(&mux);
}

void setPointer(uint16_t x, uint16_t y, bool pressed) {
    portENTER_CRITICAL(&mux);
    pointerX = x;
    pointerY = y;
    pointerPressed = pressed;
    releaseAt = 0;
    portEXIT_CRITICAL(&mux);
}

bool read(uint16_t& x, uint16_t& y, bool& pressed) {
    portENTER_CRITICAL(&mux);
    if (pointerPressed && releaseAt != 0 && static_cast<int32_t>(millis() - releaseAt) >= 0) {
        pointerPressed = false;
        releaseAt = 0;
    }
    x = pointerX;
    y = pointerY;
    pressed = pointerPressed;
    const bool active = pressed || releaseAt != 0;
    portEXIT_CRITICAL(&mux);
    return active;
}

} // namespace remote_input
