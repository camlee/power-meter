#include <Arduino.h>
#include <LiquidCrystal.h>

#include "hardware.h"

class Display {
private:
    int brightness;
    int appliedBrightness;

public:
    LiquidCrystal lcd;

    Display();
    void setup();
    void refresh();

    bool brightnessUp();
    bool brightnessDown();
    int getBrightness();
};