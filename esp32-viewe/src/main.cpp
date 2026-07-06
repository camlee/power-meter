#include <Arduino.h>
#include <WiFi.h>
#include <esp_display_panel.hpp>
#include <lvgl.h>
#include "board_setup.h"

#include "lvgl_v8_port.h"
#include "ui/screen_manager.h"
#include "ui/screen_realtime.h"
#include "ui/screen_info.h"
#include "ui/screen_wifi.h"
#include "sensors/sensor_task.h"

using namespace esp_panel::board;
using namespace esp_panel::drivers;

void setup()
{
    delay(5000);
    Serial.begin(115200);
    Board* board = initDisplayAndLvgl();
    WiFi.mode(WIFI_STA);

    Serial.println("Creating UI");
    lvgl_port_lock(-1);
    ScreenManager& screens = ScreenManager::instance();
    screens.registerScreen(ScreenId::Realtime, "Realtime", screen_realtime::create);
    screens.registerScreen(ScreenId::Info, "Info", screen_info::create);
    screens.registerScreen(ScreenId::WiFi, "WiFi", screen_wifi::create);
    screens.navigateTo(ScreenId::Realtime);
    lvgl_port_unlock();
}

void loop()
{
    delay(1000);
    Serial.println("loop()");
}
