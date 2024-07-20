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
#define TIME_PAGE 6
// #define HISTORY_PAGE -1
#define MAX_PAGE 6

#define DEFAULT_PAGE TIME_PAGE

#define refreshPeriodMillis 800

const char* weekday_to_text(byte);


class UI {
private:
    byte page;
    byte subpage;
    bool editing;
    unsigned long nextRefresh;
    bool blinkVisibility;

    byte weekday; // 0 for unknown, 1 for Sunday, 2 Monday, ... 7 for Saturday
    byte hour;
    byte minute;
    unsigned long timeSetAt;

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
    void redrawTimePage();

    void partialDrawSensor(float, float, float, float, float, bool);
    void printTime(byte, byte, byte, bool=false, bool=false, bool=false);
    boolean getBlink();

    void redrawASAP();
    void delayRedraw();


public:
    UI(Display*, SensorManager*, Store*);
    void setup();
    void refresh();
    void handleButton(int);

    byte getWeekday(unsigned long);
    byte getHour(unsigned long);
    byte getMinute(unsigned long);


};

#endif