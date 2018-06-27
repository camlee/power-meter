#include <Arduino.h>

#include "hardware.h"

#define LOAD 0
#define PANEL 1

class SensorManager {
private:
    float voltage[2];
    float current[2];
    float energy[2];
    unsigned long nextRead;

public:
    SensorManager();
    void setup();
    void refresh();

    float getPower(int);
    float getVoltage(int);
    float getCurrent(int);
    float getEnergy(int);
};