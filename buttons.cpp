// Adapted from https://www.dfrobot.com/wiki/index.php/Arduino_LCD_KeyPad_Shield_(SKU:_DFR0009)

#include "buttons.h"

const unsigned long debounceDelay = 50;

int readButton(){
    int adc_key_in = analogRead(BUTTONS_PIN);
    // my buttons when read are centered at these valies: 0, 144, 329, 504, 741
    // we add approx 50 to those values and check to see if we are close
    if (adc_key_in > 1000) return NONE_BUTTON; // We make this the 1st option for speed reasons since it will be the most likely result
    if (adc_key_in < 50)   return RIGHT_BUTTON;
    if (adc_key_in < 250)  return UP_BUTTON;
    if (adc_key_in < 450)  return DOWN_BUTTON;
    if (adc_key_in < 650)  return LEFT_BUTTON;
    if (adc_key_in < 850)  return SELECT_BUTTON;

    return NONE_BUTTON;  // when all others fail, return this...
}

ButtonManager::ButtonManager() : previousButton(NO_BUTTON), currentButton(NO_BUTTON), buttonPopped(true){}

void ButtonManager::setup(){};

void ButtonManager::refresh(){
    int button;

    button = readButton();

    if (button != previousButton){
        lastDebounceTime = millis();
    } else if (millis() - lastDebounceTime > debounceDelay){ // TODO: Handle overflow
        if (button != currentButton){
            currentButton = button;
            buttonPopped = false;
        }
    }

    previousButton = button;
}

int ButtonManager::popPressedButton(){
    if (!buttonPopped){
        buttonPopped = true;
        return currentButton;
    } else {
        return NO_BUTTON;
    }
}