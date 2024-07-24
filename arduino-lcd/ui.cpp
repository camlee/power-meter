#include "ui.h"
#include "buttons.h"
#include "util.h"

#include "pages/summary.h"
#include "pages/time.h"
#include "pages/energy.h"
#include "pages/sensors.h"
#include "pages/battery.h"


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
    if (page == LOAD_PAGE) redrawSensorPage(LOAD, display, sensorManager);
    if (page == PANEL_PAGE) redrawSensorPage(PANEL, display, sensorManager);
    if (page == BATTERY_PAGE) redrawBatteryPage(display, sensorManager);
    if (page == ENERGY_PAGE) redrawEnergyPage(display, sensorManager);
    if (page == DEBUG_PAGE) redrawDebugPage();
    if (page == TIME_PAGE) redrawTimePage(display);
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

bool UI::handleButtonResult(int result){
    if (result == HANDLED){
        return true;
    }

    if (result == REDRAW_NOW){
        redrawNow();
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
    if (page != PANEL_PAGE && page != LOAD_PAGE) resetSensorPage();

    if (page == TIME_PAGE && handleButtonResult(buttonsTimePage(button))) return;
    if (page == ENERGY_PAGE && handleButtonResult(buttonsEnergyPage(button, display, sensorManager, store))) return;
    if (page == PANEL_PAGE && handleButtonResult(buttonsSensorPage(PANEL, button, display, store))) return;
    if (page == LOAD_PAGE && handleButtonResult(buttonsSensorPage(LOAD, button, display, store))) return;

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


    // Cycle between pages:
    if (button == LEFT_BUTTON){
        page = decr_with_wrap(page, MIN_PAGE, MAX_PAGE);
        redrawNow();
        return;
    }
    if (button == RIGHT_BUTTON){
        page = incr_with_wrap(page, MIN_PAGE, MAX_PAGE);
        redrawNow();
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

void UI::redrawNow(){
    nextRefresh = millis();
}

void UI::delayRedraw(){
    nextRefresh = millis() + 2000;
}