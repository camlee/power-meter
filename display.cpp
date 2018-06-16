#include "display.h"

#define DISPLAY_NOTHING "                "

const int brightnessIncrements = 10;

Display::Display() : brightness(1), appliedBrightness(-1), lcd(LIQUID_CRYSTAL_ARGS){}

void Display::setup(){
    pinMode(BACKLIGHT_PIN, OUTPUT);
    refresh();

    lcd.begin(16, 2);
}


bool Display::brightnessUp(){
    if (brightness < brightnessIncrements){
        brightness += 1;
        return true;
    } else {
        return false;
    }
}

bool Display::brightnessDown(){
    if (brightness > 0){
        brightness -= 1;
        return true;
    } else {
        return false;
    }
}

int Display::getBrightness(){
    return brightness;
}

void Display::refresh(){
    if (appliedBrightness != brightness){
        appliedBrightness = brightness;
        analogWrite(BACKLIGHT_PIN, brightness * 255 / brightnessIncrements);
    }
}