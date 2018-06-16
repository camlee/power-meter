#include <Arduino.h>

#define RIGHT_BUTTON  0
#define UP_BUTTON     1
#define DOWN_BUTTON   2
#define LEFT_BUTTON   3
#define SELECT_BUTTON 4
#define NONE_BUTTON   5

#define NO_BUTTON -1

#define BUTTONS_PIN 0

// typedef void (*onButtonCallback)();

class ButtonManager {
private:
    int previousButton;
    int currentButton;
    unsigned long lastDebounceTime;

    bool buttonPopped;

public:
    // void onButton(int button, onButtonCallback callback);
    ButtonManager();
    void setup();
    void refresh();

    int popPressedButton();
};