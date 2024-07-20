#ifndef POWER_METER_UI_HEADER
#define POWER_METER_UI_HEADER

#include "display.h"
#include "sensor.h"
#include "store.h"


#define MIN_PAGE 0
#define SUMMARY_PAGE 0
#define ENERGY_PAGE 1
#define PANEL_PAGE 2
#define AC_PAGE 3
#define DETAILS_PAGE 4
#define DEBUG_PAGE 5
// #define HISTORY_PAGE -1
#define MAX_PAGE 5

#define DEFAULT_PAGE SUMMARY_PAGE

#define refreshPeriodMillis 800


class UI {
private:
    byte page;
    unsigned long nextRefresh;

    Display* display;
    SensorManager* sensorManager;
    Store* store;

    void redraw();
    void redrawSummaryPage();
    void redrawACPage();
    void redrawPanelPage();
    void redrawEnergyPage();
    void redrawDebugPage();
    void redrawDetailsPage();

    void partialDrawSensor(float, float, float, float, float, bool);


public:
    UI(Display*, SensorManager*, Store*);
    void setup();
    void refresh();
    void handleButton(int);
};

#endif