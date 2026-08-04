#include "board_setup.h"
#include "lvgl_v8_port.h"
#include <Arduino.h>
#include <driver/gpio.h>

using namespace esp_panel::drivers;
using namespace esp_panel::board;

namespace {

constexpr gpio_num_t kTouchResetPin = GPIO_NUM_2;

bool pulseTouchReset() {
    if (gpio_set_direction(kTouchResetPin, GPIO_MODE_OUTPUT) != ESP_OK ||
        gpio_set_level(kTouchResetPin, 0) != ESP_OK) {
        return false;
    }
    // UEDX32480035E-WB-A TP-RST is GPIO2, active low. CHSC6540 requires
    // at least 500 us low and can take up to 200 ms to resume reporting.
    delay(5);
    if (gpio_set_level(kTouchResetPin, 1) != ESP_OK) return false;
    delay(250);
    return true;
}

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

    if (!pulseTouchReset()) {
        Serial.println("touch: Could not reset CHSC6540 during startup");
    }
    assert(board->begin());

    Serial.println("Initializing LVGL");
    lvgl_port_init(board->getLCD(), board->getTouch());

    // if (lvgl_port_lock(-1)) {
    //     lv_disp_set_rotation(lv_disp_get_default(), LV_DISP_ROT_90); // 0, 90, 180, 270
    //     lvgl_port_unlock();
    // }

    return board;
}

bool resetTouchController() {
    return pulseTouchReset();
}
