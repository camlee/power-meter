#include <Arduino.h>
#include <esp_display_panel.hpp>
#include <lvgl.h>
#include <LittleFS.h>
#include "board_setup.h"

#include "lvgl_v8_port.h"
#include "ui/screen_manager.h"
#include "ui/screen_realtime.h"
#include "ui/screen_info.h"
#include "ui/screen_wifi.h"
#include "ui/screen_test.h"
#include "sensors/sensor_task.h"

using namespace esp_panel::board;
using namespace esp_panel::drivers;

void setup()
{
    Serial.begin(115200);
    LittleFS.begin(true);
    sensor_task::start();
    Board* board = initDisplayAndLvgl();

    lvgl_port_lock(-1);

    ScreenManager& screens = ScreenManager::instance();
    screens.init(); // Creates the persistent layout

    // Register screens in the order you want them to swipe
    screens.registerScreen("Realtime", screen_realtime::create);
    screens.registerScreen("WiFi", screen_wifi::create);
    screens.registerScreen("Info", screen_info::create);
    screens.registerScreen("Test", screen_test::create);

    screens.build(); // Builds tiles and dots

    lvgl_port_unlock();
}

void loop()
{
    delay(1000);
    Serial.println("loop()");
}
