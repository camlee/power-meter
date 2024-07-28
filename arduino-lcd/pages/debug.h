#include <Arduino.h>

#include "../display.h"
#include "../store.h"
#include "../sensor.h"

#include "../ui.h"


void redrawDebugPage(Display *display, Store *store){
    unsigned long uptime = millis() / 60000; // In minutes
    display->lcd.setCursor(0, 0);
    display->lcd.print("Uptime");
    display->leftPad(uptime, 3);
    display->lcd.print(uptime);
    display->lcd.print("m");
    display->lcd.print(DISPLAY_NOTHING);

    display->lcd.setCursor(0, 1);
    display->lcd.print("Boot");
    display->leftPad(store->getRestarts(), 2);
    display->lcd.print(store->getRestarts());

    display->lcd.print(" Write");
    display->leftPad(store->writes, 2);
    display->lcd.print(store->writes);
    display->lcd.print(DISPLAY_NOTHING);
}

int buttonsDebugPage(int button, Display *display, SensorManager *sensorManager){
    // Debug page:
    if (button == SELECT_BUTTON) {
        sensorManager->updateStore(true);
        display->printLines("Saved Energy!", "");
        return DELAY_REDRAW;
    }

    // Brightness controls:
    if (button == UP_BUTTON){
        display->brightnessUp();
    }
    if (button == DOWN_BUTTON){
        display->brightnessDown();
    }
    if (button == UP_BUTTON || button == DOWN_BUTTON){
        display->lcd.setCursor(0, 0);
        display->lcd.print("Brightness: ");
        display->lcd.print(display->getBrightness() * 100 / display->brightnessIncrements);
        display->lcd.print("%     ");

        display->printLine2("");
        return DELAY_REDRAW;
    }

    return BUBBLE;
}