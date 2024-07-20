#include "ui.h"
#include "buttons.h"

UI::UI(Display* display, SensorManager* sensorManager, Store* store) :
    page(DEFAULT_PAGE),
    nextRefresh(0),
    display(display),
    sensorManager(sensorManager),
    store(store)
{}

void UI::setup(){
}

void UI::refresh(){
    if (millis() > nextRefresh ){
        nextRefresh = millis() + refreshPeriodMillis;

        redraw();
    }
}

void UI::redraw(){
    if (page == SUMMARY_PAGE) {
        redrawSummaryPage();
    }

    if (page == AC_PAGE) {
        redrawACPage();
    }

    if (page == PANEL_PAGE) {
        redrawPanelPage();
    }

    if (page == DETAILS_PAGE) {
        redrawDetailsPage();
    }

    if (page == ENERGY_PAGE) {
        redrawEnergyPage();
    }

    if (page == DEBUG_PAGE) {
        redrawDebugPage();
    }

}


void UI::redrawSummaryPage(){
    float power = sensorManager->getPower(PANEL);
    float duty = sensorManager->getDuty(PANEL);
    float theoretical_power = round(power / duty);
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
    float bat_percent = round(sensorManager->getBatLevel() / sensorManager->getBatCapacity() * 100.0);
    display->lcd.setCursor(0, 1);
    display->lcd.print("Out");
    display->leftPad(power, 3);
    display->lcd.print(power, 0);
    display->lcd.print("W");

    display->lcd.print(" Bat");
    display->leftPad(bat_percent, 2);
    display->lcd.print(bat_percent, 0);
    display->lcd.print("%");
    display->lcd.print(DISPLAY_NOTHING);
}

void UI::redrawACPage(){
    float voltage = sensorManager->getVoltage(LOAD);
    float current = sensorManager->getCurrent(LOAD);
    float power = sensorManager->getPower(LOAD);
    float energy = sensorManager->getEnergy(LOAD);

    display->lcd.setCursor(0, 0);
    display->lcd.print("Out");

    partialDrawSensor(power, voltage, current, energy, 0, false);
}


void UI::redrawPanelPage(){
    float voltage = sensorManager->getVoltage(PANEL);
    float current = sensorManager->getCurrent(PANEL);
    float power = sensorManager->getPower(PANEL);
    float energy = sensorManager->getEnergy(PANEL);
    float duty = sensorManager->getDuty(PANEL) * 100.0;

    display->lcd.setCursor(0, 0);
    display->lcd.print("In");

    bool show_duty = duty < 90 && power > 30;
    partialDrawSensor(power, voltage, current, energy, duty, show_duty);
}

void UI::partialDrawSensor(float power, float voltage, float current, float energy, float duty, bool show_duty){
    display->leftPad(round(power), 2);
    display->lcd.print(power, 1);
    display->lcd.print("W");
    if (!show_duty){
        display->leftPad(energy / 3600.0, 4);
        display->lcd.print(energy / 3600.0, 0);
        display->lcd.print("Wh");
    } else {
        display->lcd.print(" PWM ");
        display->leftPad(duty, 1);
        display->lcd.print(duty, 0);
        display->lcd.print("%");
    }
    display->lcd.print(DISPLAY_NOTHING);

    display->lcd.setCursor(0, 1);
    display->leftPad(current, 1);
    display->lcd.print(current, 2);
    display->lcd.print(" A");
    display->leftPad(voltage, 3);
    display->lcd.print(voltage, 2);
    display->lcd.print(" V");
    display->lcd.print(DISPLAY_NOTHING);
}

void UI::redrawEnergyPage(){
    float energy = sensorManager->getEnergy(PANEL) / 3600.0;

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
}

void UI::redrawDebugPage(){
    unsigned long uptime = millis() / 60000; // In minutes
    display->lcd.setCursor(0, 0);
    display->lcd.print("Up");
    display->leftPad(uptime, 3);
    display->lcd.print(uptime);
    display->lcd.print("m");

    display->lcd.print(" Last");
    uptime = store->getLastUptime(); // In minutes
    display->leftPad(uptime, 2);
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

    //     if (page == HISTORY_PAGE){
    //         display->lcd.setCursor(0, 0);
    //         display->lcd.print("History Page");
    //         display->lcd.print(DISPLAY_NOTHING);
    //         display->lcd.setCursor(0, 1);
    //         display->lcd.print("TODO");
    //         display->lcd.print(DISPLAY_NOTHING);
    //         // energy = store->getHistoricalEnergy(PANEL) / 3600.0;

    //         // display->lcd.setCursor(0, 0);
    //         // display->lcd.print("Hist In ");
    //         // display->leftPad(energy, 4);
    //         // display->lcd.print(energy, 0);
    //         // display->lcd.print(" Wh");
    //         // display->lcd.print(DISPLAY_NOTHING);

    //         // display->lcd.setCursor(0, 1);
    //         // energy = (store->getHistoricalEnergy(LOAD) + store->getHistoricalEnergy(LOAD2)) / 3600.0;
    //         // display->lcd.print("Hist Out");
    //         // display->leftPad(energy, 4);
    //         // display->lcd.print(energy, 0);
    //         // display->lcd.print(" Wh");
    //         // display->lcd.print(DISPLAY_NOTHING);
    //     }

void UI::redrawDetailsPage(){
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
    // float bat_percent = round((sensorManager->getBatMax() - sensorManager->getBatMin()) / sensorManager->getBatCapacity() * 1000)/10;
    // display->leftPad(bat_percent, 2);
    // display->lcd.print(bat_percent, 1);
    // display->lcd.print("%");

    display->lcd.print(DISPLAY_NOTHING);
}

    // }

// void UI::redraw(){


//     int button;
//     float voltage;
//     float current;
//     float power;
//     float energy;
//     float duty;
//     float theoretical_power;
//     float bat_percent;
//     unsigned long nextRefresh = 0;
//     unsigned long uptime = 0;




//     int page = SUMMARY_PAGE;
// }

void UI::handleButton(int button){
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

        display->lcd.setCursor(0, 1);
        display->lcd.print(DISPLAY_NOTHING);

        nextRefresh = millis() + refreshPeriodMillis;
    }

    if (button == LEFT_BUTTON){
        if (page == 0){
            page = MAX_PAGE;
        } else {
            page -= 1;
        }
        nextRefresh = millis();
    }

    if (button == RIGHT_BUTTON){
        if (page == MAX_PAGE){
            page = MIN_PAGE;
        } else {
            page += 1;
        }
        nextRefresh = millis();
    }

    // if (page == DEBUG_PAGE && button == SELECT_BUTTON) {
    //     store->persistNow();
    //     display->lcd.setCursor(0, 0);
    //     display->lcd.print("Saved!");
    //     display->lcd.print(DISPLAY_NOTHING);

    //     display->lcd.setCursor(0, 1);
    //     display->lcd.print(DISPLAY_NOTHING);

    //     nextRefresh = millis() + refreshPeriodMillis*4;
    // }

    // if (page == ENERGY_PAGE && button == SELECT_BUTTON) {
    //     sensorManager->setup(0.0, 0.0);
    //     store->refresh(0.0, 0.0);
    //     store->persistNow();

    //     display->lcd.setCursor(0, 0);
    //     display->lcd.print("Energy reset!");
    //     display->lcd.print(DISPLAY_NOTHING);

    //     display->lcd.setCursor(0, 1);
    //     display->lcd.print(DISPLAY_NOTHING);

    //     nextRefresh = millis() + refreshPeriodMillis*4;
    // }
}