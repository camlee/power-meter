#include <Arduino.h>

#include "hardware.h"
#include "settings.h"

#define LOAD 0
#define PANEL 1

class SensorManager {
private:
    float voltage[2];
    float current[2];
    float energy[2];
    unsigned long nextReadTime;
    float currentOffset[2];

public:
    SensorManager();
    void setup();
    void refresh();

    void zeroCurrent(int);

    float getPower(int);
    float getVoltage(int);
    float getCurrent(int);
    float getEnergy(int);
};