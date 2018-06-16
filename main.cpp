#include <Arduino.h>

#include "buttons.h"
#include "display.h"

// int delay_time = 500;
// int analogValue;

// int voltage; // In mV
// char voltage_string[16];
// int button;
// int previous_button = NONE_BUTTON;



int button;

Display display = Display();
ButtonManager buttonManager = ButtonManager();
// int analogValue;

// int voltage; // In mV
// char voltage_string[16];
// int button;
// int previous_button = NONE_BUTTON;

void setup(){
    display.setup();
    buttonManager.setup();

    Serial.begin(9600);
}

void loop(){

    // Serial.println("Hello world!");
    // analogValue = analogRead(VOLTAGE_PIN);
    // voltage = 5000 * analogValue / 1023;
    // sprintf(voltage_string, "%d mV", voltage);
    // Serial.print(voltage_string);
    // Serial.print("0,");
    // Serial.print(voltage);
    // Serial.print(",5\n");


    // lcd.setCursor(0, 1);
    // lcd.print(voltage_string);
    // lcd.print(DISPLAY_NOTHING);

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
        display.lcd.print(display.getBrightness() * 10);
        display.lcd.print(" %");
    }

    display.refresh();
    buttonManager.refresh();
}

int main(void){
    init();
    setup();
    for (;;){
       loop();
    }
}
