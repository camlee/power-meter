#include <Arduino.h>

#include "buttons.h"
#include "display.h"
#include "sensor.h"

static unsigned long refreshPeriodMillis = 1000;

int button;
float voltage;
float current;
float power;
float energy;
float duty;
unsigned long nextRefresh = 0;

#define MIN_PAGE 0
#define SUMMARY_PAGE 0
#define AC_PAGE 1
#define DC_PAGE 2
#define PANEL_PAGE 3
#define MAX_PAGE 3

int page = SUMMARY_PAGE;

Display display = Display();
ButtonManager buttonManager = ButtonManager();
SensorManager sensorManager = SensorManager();

void setup(){
    display.setup();
    buttonManager.setup();
    sensorManager.setup();

    Serial.begin(9600);
}

void loop(){

    button = buttonManager.popPressedButton();

    if (button == UP_BUTTON){
        display.brightnessUp();
    }
    if (button == DOWN_BUTTON){
        display.brightnessDown();
    }

    if (button == UP_BUTTON || button == DOWN_BUTTON){
        display.lcd.setCursor(0, 0);
        display.lcd.print("Brightness: ");
        display.lcd.print(display.getBrightness() * 100 / display.brightnessIncrements);
        display.lcd.print("%     ");

        display.lcd.setCursor(0, 1);
        display.lcd.print(DISPLAY_NOTHING);

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

    #ifdef ENABLE_ZERO_CURRENT
        if (button == SELECT_BUTTON){
            if (page == AC_PAGE){
                sensorManager.zeroCurrent(LOAD);
            }
            if (page == DC_PAGE){
                sensorManager.zeroCurrent(LOAD2);
            }
            if (page == PANEL_PAGE){
                sensorManager.zeroCurrent(PANEL);
            }
        }
    #endif

    if (millis() > nextRefresh ){
        nextRefresh = millis() + refreshPeriodMillis;

        if (page == AC_PAGE){
            voltage = sensorManager.getVoltage(LOAD);
            current = sensorManager.getCurrent(LOAD);
            power = sensorManager.getPower(LOAD);
            energy = sensorManager.getEnergy(LOAD);

            display.lcd.setCursor(0, 0);
            display.lcd.print("AC: ");
        }

        if (page == DC_PAGE){
            voltage = sensorManager.getVoltage(LOAD2);
            current = sensorManager.getCurrent(LOAD2);
            power = sensorManager.getPower(LOAD2);
            energy = sensorManager.getEnergy(LOAD2);

            display.lcd.setCursor(0, 0);
            display.lcd.print("DC: ");
        }

        if (page == PANEL_PAGE){
            voltage = sensorManager.getVoltage(PANEL);
            current = sensorManager.getCurrent(PANEL);
            power = sensorManager.getPower(PANEL);
            duty = sensorManager.getDuty(PANEL) * 100.0;

            display.lcd.setCursor(0, 0);
            display.lcd.print("In: ");
        }

        if (page == AC_PAGE || page == DC_PAGE || page == PANEL_PAGE){
            display.leftPad((float)(int)power, 2);
            display.lcd.print((int)power);
            display.lcd.print(" W");
            if (page == AC_PAGE || page == DC_PAGE){
                display.leftPad(energy / 3600.0, 3);
                display.lcd.print(energy / 3600.0, 0);
                display.lcd.print(" Wh");
            } else if (page == PANEL_PAGE){
                display.lcd.print(" (");
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
            energy = sensorManager.getEnergy(PANEL);

            display.lcd.setCursor(0, 0);
            display.lcd.print("In: ");
            display.leftPad((float)(int)power, 2);
            display.lcd.print((int)power);
            display.lcd.print(" W");
            display.leftPad(energy / 3600.0, 3);
            display.lcd.print(energy / 3600.0, 0);
            display.lcd.print(" Wh");
            display.lcd.print(DISPLAY_NOTHING);

            power = sensorManager.getPower(LOAD) + sensorManager.getPower(LOAD2);
            energy = sensorManager.getEnergy(LOAD) + sensorManager.getEnergy(LOAD2);
            display.lcd.setCursor(0, 1);
            display.lcd.print("Out:");
            display.leftPad((float)(int)power, 2);
            display.lcd.print((int)power);
            display.lcd.print(" W");
            display.leftPad(energy / 3600.0, 3);
            display.lcd.print(energy / 3600.0, 0);
            display.lcd.print(" Wh");
            display.lcd.print(DISPLAY_NOTHING);
        }
    }

    display.refresh();
    buttonManager.refresh();
    sensorManager.refresh();
}

int main(void){
    init();
    setup();
    for (;;){
       loop();
    }
}
