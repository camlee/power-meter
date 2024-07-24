#include "../display.h"
#include "../sensor.h"

void redrawSummaryPage(Display *display, SensorManager *sensorManager){
    float power = sensorManager->getPower(PANEL);
    float duty = sensorManager->getDuty(PANEL);
    float theoretical_power = round(power / duty);
    float net_energy = (sensorManager->getEnergy(PANEL) -  (sensorManager->getEnergy(LOAD) + sensorManager->getEnergy(LOAD2))) / 3600.0;
    power = round(power);

    display->lcd.setCursor(0, 0);
    display->lcd.print("Sun");
    display->leftPad(theoretical_power, 3);
    display->lcd.print(theoretical_power, 0);
    display->lcd.print("W");

    display->lcd.print(" In");
    display->leftPad(power, 3);
    display->lcd.print(power, 0);
    display->lcd.print("W");

    display->lcd.print(DISPLAY_NOTHING);

    power = round(sensorManager->getPower(LOAD) + sensorManager->getPower(LOAD2));
    display->lcd.setCursor(0, 1);
    display->lcd.print("Out");
    display->leftPad(power, 3);
    display->lcd.print(power, 0);
    display->lcd.print("W");

    display->lcd.print(" ");
    display->leftPad(abs(net_energy), 3);
    if (net_energy > 0){
        display->lcd.print("+");
    } else {
        display->lcd.print("-");
    }
    display->lcd.print(abs(net_energy), 0);
    display->lcd.print("Wh");

    // float bat_percent = round(sensorManager->getBatLevel() / sensorManager->getBatCapacity() * 100.0);
    // display->lcd.print(" Bat");
    // display->leftPad(bat_percent, 2);
    // display->lcd.print(bat_percent, 0);
    // display->lcd.print("%");
    // display->lcd.print(DISPLAY_NOTHING);
}
