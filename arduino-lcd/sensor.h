#ifndef POWER_METER_SENSOR_HEADER
#define POWER_METER_SENSOR_HEADER

#include <Arduino.h>

#include "hardware.h"
#include "settings.h"

class SensorManager {
private:
    float voltage_factor[NUM_SENSORS];
    float current_zero[NUM_SENSORS];
    float current_factor[NUM_SENSORS];
    float voltage_buffer[NUM_SENSORS][SENSOR_READINGS_WINDOW]; // Stores pre-factor values. Not useable voltages.
    float current_buffer[NUM_SENSORS][SENSOR_READINGS_WINDOW]; // Stores pre-zero and factor values. Not useable currents.
    unsigned int buffer_index;
    float energy[NUM_SENSORS];
    unsigned long lastReadTime[NUM_SENSORS];
    unsigned long nextReadTime;
    float min_bat;
    float max_bat;

    float readingToVoltage(int, float);
    float readingToCurrent(int, float, float=NAN, float=NAN);

public:
    SensorManager();
    void setup(float, float);
    void refresh();

    float getPower(int);
    float getVoltage(int);
    float getCurrent(int, float=NAN, float=NAN);
    float getEnergy(int);
    float getDuty(int);

    float getNetEnergy();
    float getBatLevel();
    float getBatCapacity();
    float getBatMin();
    float getBatMax();

    float getCurrentZero(int);
    void setCurrentZero(int, float);
    float getCurrentFactor(int);
    void setCurrentFactor(int, float);
    float getVoltageFactor(int);
    void setVoltageFactor(int, float);
};

#endif