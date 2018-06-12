#include <Arduino.h>
#include <LiquidCrystal.h>

#include "hardware_config.h"
#include "buttons.h"

int delay_time = 500;
int analogValue;

int voltage; // In mV
char voltage_string[16];
int button;
int previous_button = NONE_BUTTON;
int brightness = 1;
int brightness_value = brightness * 255 / 10;

LiquidCrystal lcd = LiquidCrystal(8, 9, 4, 5, 6, 7);
#define BACKLIGHT_PIN 10
#define DISPLAY_NOTHING "                "

void setup(){
    pinMode(LED_PIN, OUTPUT);
    Serial.begin(9600);

    pinMode(BACKLIGHT_PIN, OUTPUT);
    analogWrite(BACKLIGHT_PIN, brightness_value);

    lcd.begin(16, 2);
}

void loop(){
    digitalWrite(LED_PIN, HIGH);   // set the LED on
    delay(delay_time);
    digitalWrite(LED_PIN, LOW);    // set the LED off
    delay(delay_time);

    // Serial.println("Hello world!");
    analogValue = analogRead(VOLTAGE_PIN);
    voltage = 5000 * analogValue / 1023;
    sprintf(voltage_string, "%d mV", voltage);
    // Serial.print(voltage_string);
    // Serial.print("0,");
    // Serial.print(voltage);
    // Serial.print(",5\n");

    lcd.setCursor(0, 1);
    lcd.print(voltage_string);
    lcd.print(DISPLAY_NOTHING);

    button = read_LCD_buttons();
    if (button != previous_button){
        previous_button = button;

        if (button == UP_BUTTON){
            if (brightness < 10){
                brightness += 1;
                brightness_value = brightness * 255 / 10;
            }
        }

        if (button == DOWN_BUTTON){
            if (brightness > 0){
                brightness -= 1;
                brightness_value = brightness * 255 / 10;
            }
        }

        if (button == UP_BUTTON || button == DOWN_BUTTON){
            lcd.setCursor(0, 0);
            lcd.print("Brightness: ");
            lcd.print(brightness * 10);
            lcd.print(" %");
            lcd.print(DISPLAY_NOTHING);
        }

        if (button == SELECT_BUTTON){
            lcd.setCursor(0, 0);
            lcd.print(DISPLAY_NOTHING);
        }

    }

    analogWrite(BACKLIGHT_PIN, brightness_value);
}

int main(void){
    init();
    setup();
    for (;;){
       loop();
    }
}
