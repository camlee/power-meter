#include "ui.h"
#include "buttons.h"
#include "util.h"

#include "pages/summary.h"
#include "pages/time.h"
#include "pages/energy.h"
#include "pages/sensors.h"
#include "pages/battery.h"
#include "pages/debug.h"

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
        nextRefresh = millis() + REFRESH_PERIOD_MILLIS;
        redraw();
    }
}

void UI::redraw(){
    if (page == SUMMARY_PAGE) redrawSummaryPage(display, sensorManager);
    if (page == LOAD_PAGE) redrawSensorPage(LOAD, display, sensorManager);
    if (page == PANEL_PAGE) redrawSensorPage(PANEL, display, sensorManager);
    if (page == BATTERY_PAGE) redrawBatteryPage(display, sensorManager);
    if (page == ENERGY_PAGE) redrawEnergyPage(display, sensorManager);
    if (page == DEBUG_PAGE) redrawDebugPage(display, store);
    if (page == TIME_PAGE) redrawTimePage(display);
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
    if (page != SUMMARY_PAGE) resetSummaryPage();

    if (page == TIME_PAGE && handleButtonResult(buttonsTimePage(button))) return;
    if (page == ENERGY_PAGE && handleButtonResult(buttonsEnergyPage(button, display, sensorManager))) return;
    if (page == SUMMARY_PAGE && handleButtonResult(buttonsSummaryPage(button, display, sensorManager))) return;
    if (page == PANEL_PAGE && handleButtonResult(buttonsSensorPage(PANEL, button, display, sensorManager))) return;
    if (page == LOAD_PAGE && handleButtonResult(buttonsSensorPage(LOAD, button, display, sensorManager))) return;
    if (page == DEBUG_PAGE && handleButtonResult(buttonsDebugPage(button, display, sensorManager))) return;

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
}

void UI::redrawNow(){
    nextRefresh = millis();
}

void UI::delayRedraw(){
    nextRefresh = millis() + 2000;
}