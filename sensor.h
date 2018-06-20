#include <Arduino.h>

#include "hardware.h"

class SensorManager {
private:
    float voltage;
    float current;

public:
    SensorManager();
    void setup();
    void refresh();

    float getPower();
    float getVoltage();
    float getCurrent();
};