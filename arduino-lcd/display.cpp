#include "display.h"

Display::Display() : brightness(1), appliedBrightness(-1), brightnessIncrements(10), lcd(LIQUID_CRYSTAL_ARGS){}

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

// void Display::persistPrint(){

// }

void Display::refresh(){
    if (appliedBrightness != brightness){
        appliedBrightness = brightness;
        analogWrite(BACKLIGHT_PIN, brightness * 255 / brightnessIncrements);
    }
}

void Display::leftPad(float value, int maxSpaces){
    int spaces = maxSpaces; // How many spaces to print. Printing at most maxSpaces
    if (value < 0.0){
        spaces -= 1; // Leaving a space for the minus sign
    }
    value = abs(value);
    while (value >= 10 && spaces > 0){
        spaces -= 1;
        value = value / 10;
    }

    while (spaces > 0){
        lcd.print(" ");
        spaces -= 1;
    }
    return;
}

void Display::printYesNoSelection(bool yes){
    bool blink = getBlink();
    if (!yes || blink) {
        lcd.print(" yes ");
    } else {
        lcd.print("     ");
    }
    if (yes || blink) {
        lcd.print(" no ");
    } else {
        lcd.print("    ");
    }
    lcd.print(DISPLAY_NOTHING);
}

bool Display::getBlink(){
    return (unsigned long)(millis() / 300) % 2 == 0;
}