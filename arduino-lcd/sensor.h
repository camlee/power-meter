#include <Arduino.h>

#include "hardware.h"
#include "settings.h"

#define LOAD 0
#define PANEL 1
#define LOAD2 2

class SensorManager {
private:
    float voltage[3];
    float current[3];
    float energy[3];
    unsigned long nextReadTime;
    float currentOffset[3];

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