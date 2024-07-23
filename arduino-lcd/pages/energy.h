#include "../display.h"
#include "../sensor.h"
#include "../store.h"

#include "../ui.h"

bool energy_main_subpage = true;
bool energy_no_selected = true;

void redrawEnergyPage(Display *display, SensorManager *sensorManager){
    float energy;
    bool blink;

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
        blink = (unsigned long)(millis() / 500) % 2 == 0;

        display->printLine1("Reset Energy?");
        display->lcd.setCursor(0, 1);

        if (!energy_no_selected || blink) {
            display->lcd.print(" no ");
        } else {
            display->lcd.print("    ");
        }

        if (energy_no_selected || blink) {
            display->lcd.print(" yes ");
        } else {
            display->lcd.print("     ");
        }
        display->lcd.print(DISPLAY_NOTHING);
    }
}

int buttonsEnergyPage(int button, Display *display, SensorManager *sensorManager, Store *store){
    if (energy_main_subpage) {
        if (button == SELECT_BUTTON){
            energy_main_subpage = false;
            return REDRAW_NOW;
        }
    } else {
        if (button == SELECT_BUTTON){
            if (!energy_no_selected){
                sensorManager->setup(0.0, 0.0);
                store->refresh(0.0, 0.0);
                store->persistNow();

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
            return HANDLED;
        }
    }

    return BUBBLE;
}

void resetEnergyPage(){
    energy_main_subpage = true;
    energy_no_selected = true;
}