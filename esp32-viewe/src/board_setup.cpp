#include "board_setup.h"
#include "lvgl_v8_port.h"
#include <Arduino.h>

using namespace esp_panel::drivers;
using namespace esp_panel::board;

namespace {

void configureTearingAvoidance(Board* board) {
#if LVGL_PORT_AVOID_TEARING_MODE
    Serial.println("LVGL_PORT_AVOID_TEARING_MODE");
    auto lcd = board->getLCD();
    lcd->configFrameBufferNumber(LVGL_PORT_DISP_BUFFER_NUM);
#if ESP_PANEL_DRIVERS_BUS_ENABLE_RGB && CONFIG_IDF_TARGET_ESP32S3
    Serial.println("ESP_PANEL_DRIVERS_BUS_ENABLE_RGB && CONFIG_IDF_TARGET_ESP32S3");
    auto lcd_bus = lcd->getBus();
    if (lcd_bus->getBasicAttributes().type == ESP_PANEL_BUS_TYPE_RGB) {
        static_cast<BusRGB *>(lcd_bus)->configRGB_BounceBufferSize(lcd->getFrameWidth() * 10);
    }
#endif
#endif
}

} // namespace

Board* initDisplayAndLvgl() {
    Serial.println("Initializing board");
    Board* board = new Board();
    board->init();

    configureTearingAvoidance(board);

    assert(board->begin());

    Serial.println("Initializing LVGL");
    lvgl_port_init(board->getLCD(), board->getTouch());

    return board;
}
