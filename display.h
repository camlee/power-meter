#include <Arduino.h>
#include <LiquidCrystal.h>

#include "hardware.h"

#define DISPLAY_NOTHING "                "

class Display {
private:
    int brightness;
    int appliedBrightness;

public:
    LiquidCrystal lcd;

    const int brightnessIncrements;

    Display();
    void setup();
    void refresh();

    bool brightnessUp();
    bool brightnessDown();
    int getBrightness();
};