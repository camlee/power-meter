#include <Arduino.h>

#include "hardware.h"
#include "settings.h"

#define LOAD 0
#define PANEL 1
#define LOAD2 2
#define NUM_SENSORS 3

class SensorManager {
private:
    float voltage_buffer[NUM_SENSORS][SENSOR_READINGS_WINDOW];
    float current_buffer[NUM_SENSORS][SENSOR_READINGS_WINDOW];
    unsigned int buffer_index;
    float energy[NUM_SENSORS];
    unsigned long lastReadTime[NUM_SENSORS];
    unsigned long nextReadTime;
    float currentOffset[NUM_SENSORS];

public:
    SensorManager();
    void setup();
    void refresh();

    void zeroCurrent(int);

    float getPower(int);
    float getVoltage(int);
    float getCurrent(int);
    float getEnergy(int);
    float getDuty(int);
};