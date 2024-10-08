#ifndef POWER_METER_DISPLAY_HEADER
#define POWER_METER_DISPLAY_HEADER

#include <Arduino.h>
#include <LiquidCrystal.h>

#include "hardware.h"

#define DISPLAY_NOTHING "                "

class Display {
private:
    int brightness;
    int appliedBrightness;

public:
    const int brightnessIncrements;

    LiquidCrystal lcd;

    Display();
    void setup();
    void refresh();

    bool brightnessUp();
    bool brightnessDown();
    int getBrightness();

    template<class TYPE>
    void printLine(TYPE text){
        lcd.print(text);
        lcd.print(DISPLAY_NOTHING);
    }

    template<class TYPE>
    void printLine1(TYPE text){
        lcd.setCursor(0, 0);
        printLine(text);
    }

    template<class TYPE>
    void printLine2(TYPE text){
        lcd.setCursor(0, 1);
        printLine(text);
    }

    template<class TYPE1, class TYPE2>
    void printLines(TYPE1 text1, TYPE2 text2){
        printLine1(text1);
        printLine2(text2);
    }


    void leftPad(float value, int maxSpaces);
    void printYesNoSelection(bool);

    bool getBlink();
};

#endif