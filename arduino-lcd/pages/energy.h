#include "../display.h"
#include "../sensor.h"
#include "../ui.h"

bool energy_main_subpage = true;
bool energy_no_selected = true;

void redrawEnergyPage(Display *display, SensorManager *sensorManager){
    float energy;

    if (energy_main_subpage){
        energy = sensorManager->getEnergy(PANEL) / 3600.0;
        display->lcd.setCursor(0, 0);
        display->lcd.print("In ");
        display->leftPad(energy, 8);
        display->lcd.print(energy, 0);
        display->lcd.print(" Wh");
        display->lcd.print(DISPLAY_NOTHING);

        display->lcd.setCursor(0, 1);
        energy = (sensorManager->getEnergy(LOAD) + sensorManager->getEnergy(LOAD2)) / 3600.0;
        display->lcd.print("Out");
        display->leftPad(energy, 8);
        display->lcd.print(energy, 0);
        display->lcd.print(" Wh");
        display->lcd.print(DISPLAY_NOTHING);
    } else {

        display->printLine1("Reset Energy?");
        display->lcd.setCursor(0, 1);

        display->printYesNoSelection(!energy_no_selected);
    }
}

int buttonsEnergyPage(int button, Display *display, SensorManager *sensorManager){
    if (energy_main_subpage) {
        if (button == SELECT_BUTTON){
            energy_main_subpage = false;
            return REDRAW_NOW;
        }
    } else {
        if (button == SELECT_BUTTON){
            if (!energy_no_selected){
                sensorManager->resetEnergy();

                energy_main_subpage = true;
                energy_no_selected = true;
                display->printLines("Energy reset!", "");
                return DELAY_REDRAW;
            } else {
                energy_main_subpage = true;
                return REDRAW_NOW;
            }
        }
        if (button == LEFT_BUTTON || button == RIGHT_BUTTON) {
            energy_no_selected = !energy_no_selected;
            return REDRAW_NOW;
        }
    }

    return BUBBLE;
}

void resetEnergyPage(){
    energy_main_subpage = true;
    energy_no_selected = true;
}