#include "ui.h"
#include "buttons.h"
#include "util.h"

#include "pages/summary.h"
#include "pages/time.h"
#include "pages/energy.h"


UI::UI(Display* display, SensorManager* sensorManager, Store* store) :
    page(DEFAULT_PAGE),
    nextRefresh(0),

    display(display),
    sensorManager(sensorManager),
    store(store)
{}

void UI::setup(){}

void UI::refresh(){
    if (millis() > nextRefresh ){
        nextRefresh = millis() + refreshPeriodMillis;
        redraw();
    }
}

void UI::redraw(){
    if (page == SUMMARY_PAGE) redrawSummaryPage(display, sensorManager);
    if (page == AC_PAGE) redrawACPage();
    if (page == PANEL_PAGE) redrawPanelPage();
    if (page == DETAILS_PAGE) redrawDetailsPage();
    if (page == ENERGY_PAGE) redrawEnergyPage(display, sensorManager);
    if (page == DEBUG_PAGE) redrawDebugPage();
    if (page == TIME_PAGE) redrawTimePage(display);
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

bool UI::handleButtonResult(int result){
    if (result == HANDLED){
        return true;
    }

    if (result == REDRAW_NOW){
        redrawASAP();
        return true;
    }

    if (result == DELAY_REDRAW){
        delayRedraw();
        return true;
    }

    return false;
}


void UI::handleButton(int button){
    // Order of code matters here, we return early when applying a button press.
    // More general near the bottom. More specific near the top.
    // This is to avoid a button press doing multiple things and having to
    // code all specific cases.

    if (page != ENERGY_PAGE) resetEnergyPage();

    // Debug page:
    if (page == DEBUG_PAGE && button == SELECT_BUTTON) {
        store->persistNow();
        display->lcd.setCursor(0, 0);
        display->lcd.print("Saved!");
        display->lcd.print(DISPLAY_NOTHING);

        display->lcd.setCursor(0, 1);
        display->lcd.print(DISPLAY_NOTHING);

        delayRedraw();
        return;
    }

    if (page == TIME_PAGE && handleButtonResult(buttonsTimePage(button))) return;
    if (page == ENERGY_PAGE && handleButtonResult(buttonsEnergyPage(button, display, sensorManager, store))) return;

    // Cycle between pages:
    if (button == LEFT_BUTTON){
        page = decr_with_wrap(page, MIN_PAGE, MAX_PAGE);
        redrawASAP();
        return;
    }
    if (button == RIGHT_BUTTON){
        page = incr_with_wrap(page, MIN_PAGE, MAX_PAGE);
        redrawASAP();
        return;
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

        display->lcd.setCursor(0, 1);
        display->lcd.print(DISPLAY_NOTHING);

        delayRedraw();
        return;
    }

}

void UI::redrawASAP(){
    nextRefresh = millis();
}

void UI::delayRedraw(){
    nextRefresh = millis() + 2000;
}