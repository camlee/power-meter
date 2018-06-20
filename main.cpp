#include <Arduino.h>

#include "buttons.h"
#include "display.h"
#include "sensor.h"

static unsigned long refreshPeriodMillis = 1000;

int button;
float voltage;
float current;
float power;
unsigned long nextRefresh = 0;

Display display = Display();
ButtonManager buttonManager = ButtonManager();
SensorManager sensorManager = SensorManager();

void setup(){
    display.setup();
    buttonManager.setup();
    sensorManager.setup();

    // Serial.begin(9600);
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

        nextRefresh = millis() + refreshPeriodMillis;
    }

    if (millis() > nextRefresh ){
        nextRefresh = millis() + refreshPeriodMillis;

        voltage = sensorManager.getVoltage();
        current = sensorManager.getCurrent();
        power = sensorManager.getPower();

        display.lcd.setCursor(0, 0);
        display.lcd.print(power);
        display.lcd.print(" W");
        display.lcd.print(DISPLAY_NOTHING);

        display.lcd.setCursor(0, 1);
        display.lcd.print(current * 1000.0, 0);
        display.lcd.print(" mA, ");
        display.lcd.print(voltage, 1);
        display.lcd.print(" V");
        display.lcd.print(DISPLAY_NOTHING);
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
