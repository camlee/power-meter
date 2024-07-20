#ifndef POWER_METER_SENSOR_HEADER
#define POWER_METER_SENSOR_HEADER

#include <Arduino.h>

#include "hardware.h"
#include "settings.h"

class SensorManager {
private:
    float voltage_buffer[NUM_SENSORS][SENSOR_READINGS_WINDOW];
    float current_buffer[NUM_SENSORS][SENSOR_READINGS_WINDOW];
    unsigned int buffer_index;
    float energy[NUM_SENSORS];
    unsigned long lastReadTime[NUM_SENSORS];
    unsigned long nextReadTime;
    float min_bat;
    float max_bat;

public:
    SensorManager();
    void setup(float, float);
    void refresh();

    float getPower(int);
    float getVoltage(int);
    float getCurrent(int);
    float getEnergy(int);
    float getDuty(int);

    float getNetEnergy();
    float getBatLevel();
    float getBatCapacity();
    float getBatMin();
    float getBatMax();
};

#endif