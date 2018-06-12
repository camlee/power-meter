#include <Arduino.h>

#define RIGHT_BUTTON  0
#define UP_BUTTON     1
#define DOWN_BUTTON   2
#define LEFT_BUTTON   3
#define SELECT_BUTTON 4
#define NONE_BUTTON   5

#ifndef BUTTONS_PIN
    #define BUTTONS_PIN 0
#endif

int read_LCD_buttons();