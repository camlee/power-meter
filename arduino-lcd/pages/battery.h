#include "../display.h"
#include "../sensor.h"

#include "../ui.h"

void redrawBatteryPage(Display *display, SensorManager *sensorManager){
    display->lcd.setCursor(0, 0);
    display->lcd.print("Bat ");
    display->lcd.print(sensorManager->getBatLevel() / 3600.0, 0);
    display->lcd.print("/");
    display->lcd.print(sensorManager->getBatCapacity() / 3600.0, 0);
    display->lcd.print(" Wh");
    display->lcd.print(DISPLAY_NOTHING);

    display->lcd.setCursor(0, 1);
    display->lcd.print(sensorManager->getBatMin() / 3600.0, 0);
    display->lcd.print("<->");
    display->lcd.print(sensorManager->getBatMax() / 3600.0, 0);
    display->lcd.print("Wh");

    float bat_percent = round(sensorManager->getBatLevel() / sensorManager->getBatCapacity() * 100.0);
    display->leftPad(bat_percent, 2);
    display->lcd.print(bat_percent, 0);
    display->lcd.print("%");
    display->lcd.print(DISPLAY_NOTHING);
}
