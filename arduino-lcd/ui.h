#ifndef POWER_METER_UI_HEADER
#define POWER_METER_UI_HEADER

#include "display.h"
#include "sensor.h"
#include "store.h"

// Pages:
#define MIN_PAGE 0
#define SUMMARY_PAGE 0
#define ENERGY_PAGE 1
#define PANEL_PAGE 2
#define LOAD_PAGE 3
#define BATTERY_PAGE 4
#define DEBUG_PAGE 5
#define TIME_PAGE 6
// #define HISTORY_PAGE -1
#define MAX_PAGE 6

#define DEFAULT_PAGE SUMMARY_PAGE

#define refreshPeriodMillis 100

// Results of handling button:
#define BUBBLE 0
#define HANDLED 1
#define REDRAW_NOW 2 // Implies HANDLED
#define DELAY_REDRAW 3 // Implies HANDLED

class UI {
private:
    byte page;
    unsigned long nextRefresh;

    Display* display;
    SensorManager* sensorManager;
    Store* store;

    void redraw();

    void redrawNow();
    void delayRedraw();


public:
    UI(Display*, SensorManager*, Store*);
    void setup();
    void refresh();
    void handleButton(int);
    bool handleButtonResult(int);
};

#endif