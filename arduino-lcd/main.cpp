#include <Arduino.h>

#include "buttons.h"
#include "display.h"
#include "sensor.h"
#include "store.h"

static unsigned long refreshPeriodMillis = 500;

int button;
float voltage;
float current;
float power;
float energy;
float duty;
float theoretical_power;
float bat_percent;
unsigned long nextRefresh = 0;
unsigned long uptime = 0;

#define MIN_PAGE 0
#define SUMMARY_PAGE 0
#define AC_PAGE 1
#define PANEL_PAGE 2
#define DETAILS_PAGE 3
#define ENERGY_PAGE 4
#define DEBUG_PAGE 5
#define HISTORY_PAGE -1
#define MAX_PAGE 5
// Disabled pages:
#define DC_PAGE -1


int page = SUMMARY_PAGE;

Display display = Display();
ButtonManager buttonManager = ButtonManager();
SensorManager sensorManager = SensorManager();
Store store = Store();

void setup(){
    Serial.begin(9600);

    store.setup();
    display.setup();
    buttonManager.setup();
    sensorManager.setup(
        store.getSavedEnergyPanel(),
        store.getSavedEnergyLoad()
        );
}


void loop(){

    button = buttonManager.popPressedButton();

    // if (button == UP_BUTTON){
    //     display.brightnessUp();
    // }
    // if (button == DOWN_BUTTON){
    //     display.brightnessDown();
    // }

    // if (button == UP_BUTTON || button == DOWN_BUTTON){
    //     display.lcd.setCursor(0, 0);
    //     display.lcd.print("Brightness: ");
    //     display.lcd.print(display.getBrightness() * 100 / display.brightnessIncrements);
    //     display.lcd.print("%     ");

    //     display.lcd.setCursor(0, 1);
    //     display.lcd.print(DISPLAY_NOTHING);

    //     nextRefresh = millis() + refreshPeriodMillis;
    // }

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

    if (page == DEBUG_PAGE && button == SELECT_BUTTON) {
        store.persistNow();
        display.lcd.setCursor(0, 0);
        display.lcd.print("Saved!");
        display.lcd.print(DISPLAY_NOTHING);

        display.lcd.setCursor(0, 1);
        display.lcd.print(DISPLAY_NOTHING);

        nextRefresh = millis() + refreshPeriodMillis*4;
    }

    if (page == ENERGY_PAGE && button == SELECT_BUTTON) {
        sensorManager.setup(0.0, 0.0);
        store.refresh(0.0, 0.0);
        store.persistNow();

        display.lcd.setCursor(0, 0);
        display.lcd.print("Energy reset!");
        display.lcd.print(DISPLAY_NOTHING);

        display.lcd.setCursor(0, 1);
        display.lcd.print(DISPLAY_NOTHING);

        nextRefresh = millis() + refreshPeriodMillis*4;
    }

    if (millis() > nextRefresh ){
        nextRefresh = millis() + refreshPeriodMillis;

        if (page == AC_PAGE){
            voltage = sensorManager.getVoltage(LOAD);
            current = sensorManager.getCurrent(LOAD);
            power = sensorManager.getPower(LOAD);
            energy = sensorManager.getEnergy(LOAD);

            display.lcd.setCursor(0, 0);
            display.lcd.print("Out");
        }

        if (page == DC_PAGE){
            voltage = sensorManager.getVoltage(LOAD2);
            current = sensorManager.getCurrent(LOAD2);
            power = sensorManager.getPower(LOAD2);
            energy = sensorManager.getEnergy(LOAD2);

            display.lcd.setCursor(0, 0);
            display.lcd.print("DC ");
        }

        if (page == PANEL_PAGE){
            voltage = sensorManager.getVoltage(PANEL);
            current = sensorManager.getCurrent(PANEL);
            power = sensorManager.getPower(PANEL);
            duty = sensorManager.getDuty(PANEL) * 100.0;

            display.lcd.setCursor(0, 0);
            display.lcd.print("In");
        }

        if (page == AC_PAGE || page == DC_PAGE || page == PANEL_PAGE){
            display.leftPad(round(power), 2);
            display.lcd.print(power, 1);
            display.lcd.print("W");
            if (page == AC_PAGE || page == DC_PAGE){
                display.leftPad(energy / 3600.0, 4);
                display.lcd.print(energy / 3600.0, 0);
                display.lcd.print("Wh");
            } else if (page == PANEL_PAGE){
                display.lcd.print("  (");
                display.leftPad(duty, 2);
                display.lcd.print(duty, 0);
                display.lcd.print("%)");
            }
            display.lcd.print(DISPLAY_NOTHING);

            display.lcd.setCursor(0, 1);
            display.leftPad(current, 1);
            display.lcd.print(current, 2);
            display.lcd.print(" A");
            display.leftPad(voltage, 3);
            display.lcd.print(voltage, 2);
            display.lcd.print(" V");
            display.lcd.print(DISPLAY_NOTHING);
        }

        if (page == SUMMARY_PAGE){
            power = sensorManager.getPower(PANEL);
            duty = sensorManager.getDuty(PANEL);
            theoretical_power = round(power / duty);
            power = round(power);

            display.lcd.setCursor(0, 0);
            display.lcd.print("Sun");
            display.leftPad(theoretical_power, 3);
            display.lcd.print(theoretical_power, 0);
            display.lcd.print("W");

            display.lcd.print(" In");
            display.leftPad(power, 3);
            display.lcd.print(power, 0);
            display.lcd.print("W");

            display.lcd.print(DISPLAY_NOTHING);

            power = round(sensorManager.getPower(LOAD) + sensorManager.getPower(LOAD2));
            bat_percent = round(sensorManager.getBatLevel() / sensorManager.getBatCapacity() * 100.0);
            display.lcd.setCursor(0, 1);
            display.lcd.print("Out");
            display.leftPad(power, 3);
            display.lcd.print(power, 0);
            display.lcd.print("W");

            display.lcd.print(" Bat");
            display.leftPad(bat_percent, 2);
            display.lcd.print(bat_percent, 0);
            display.lcd.print("%");
            display.lcd.print(DISPLAY_NOTHING);
        }

        if (page == ENERGY_PAGE){
            energy = sensorManager.getEnergy(PANEL) / 3600.0;

            display.lcd.setCursor(0, 0);
            display.lcd.print("In ");
            display.leftPad(energy, 8);
            display.lcd.print(energy, 0);
            display.lcd.print(" Wh");
            display.lcd.print(DISPLAY_NOTHING);

            display.lcd.setCursor(0, 1);
            energy = (sensorManager.getEnergy(LOAD) + sensorManager.getEnergy(LOAD2)) / 3600.0;
            display.lcd.print("Out");
            display.leftPad(energy, 8);
            display.lcd.print(energy, 0);
            display.lcd.print(" Wh");
            display.lcd.print(DISPLAY_NOTHING);
        }

        if (page == DEBUG_PAGE){
            uptime = millis() / 60000; // In minutes
            display.lcd.setCursor(0, 0);
            display.lcd.print("Up");
            display.leftPad(uptime, 3);
            display.lcd.print(uptime);
            display.lcd.print("m");
            display.lcd.print(" Last");
            uptime = store.getLastUptime(); // In minutes
            display.leftPad(uptime, 2);
            display.lcd.print(uptime);
            display.lcd.print("m");
            display.lcd.print(DISPLAY_NOTHING);

            display.lcd.setCursor(0, 1);
            display.lcd.print("Boot");
            display.leftPad(store.getRestarts(), 2);
            display.lcd.print(store.getRestarts());
            display.lcd.print(" Write");
            display.leftPad(store.writes, 2);
            display.lcd.print(store.writes);
            display.lcd.print(DISPLAY_NOTHING);
        }

        if (page == HISTORY_PAGE){
            display.lcd.setCursor(0, 0);
            display.lcd.print("History Page");
            display.lcd.print(DISPLAY_NOTHING);
            display.lcd.setCursor(0, 1);
            display.lcd.print("TODO");
            display.lcd.print(DISPLAY_NOTHING);
            // energy = store.getHistoricalEnergy(PANEL) / 3600.0;

            // display.lcd.setCursor(0, 0);
            // display.lcd.print("Hist In ");
            // display.leftPad(energy, 4);
            // display.lcd.print(energy, 0);
            // display.lcd.print(" Wh");
            // display.lcd.print(DISPLAY_NOTHING);

            // display.lcd.setCursor(0, 1);
            // energy = (store.getHistoricalEnergy(LOAD) + store.getHistoricalEnergy(LOAD2)) / 3600.0;
            // display.lcd.print("Hist Out");
            // display.leftPad(energy, 4);
            // display.lcd.print(energy, 0);
            // display.lcd.print(" Wh");
            // display.lcd.print(DISPLAY_NOTHING);
        }

        if (page == DETAILS_PAGE){
            display.lcd.setCursor(0, 0);
            display.lcd.print("Bat ");
            display.lcd.print(sensorManager.getBatLevel() / 3600.0, 0);
            display.lcd.print("/");
            display.lcd.print(sensorManager.getBatCapacity() / 3600.0, 0);
            display.lcd.print(" Wh");
            display.lcd.print(DISPLAY_NOTHING);

            display.lcd.setCursor(0, 1);
            display.lcd.print(sensorManager.getBatMin() / 3600.0, 0);
            display.lcd.print("<->");
            display.lcd.print(sensorManager.getBatMax() / 3600.0, 0);
            display.lcd.print("Wh");
            bat_percent = round((sensorManager.getBatMax() - sensorManager.getBatMin()) / sensorManager.getBatCapacity() * 1000)/10;
            display.leftPad(bat_percent, 2);
            display.lcd.print(bat_percent, 1);
            display.lcd.print("%");

            display.lcd.print(DISPLAY_NOTHING);
        }

    }

    display.refresh();
    buttonManager.refresh();
    sensorManager.refresh();
    store.refresh(
        sensorManager.getEnergy(PANEL),
        sensorManager.getEnergy(LOAD)
        );

}

int main(void){
    init();
    setup();
    for (;;){
       loop();
    }
}
