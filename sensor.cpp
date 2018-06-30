#include "sensor.h"

#define FACTOR SENSOR_ADC_FACTOR

SensorManager::SensorManager() : voltage(), current(), energy(), nextRead(0){}

void SensorManager::setup(){

}

void SensorManager::refresh(){
    if (millis() > nextRead){
        #ifdef LOAD_VOLTAGE_PIN
            voltage[LOAD] = float(analogRead(LOAD_VOLTAGE_PIN)) * FACTOR * LOAD_VOLTAGE_FACTOR;
        #endif
        #ifdef LOAD_CURRENT_PIN
            current[LOAD] = ((float(analogRead(LOAD_CURRENT_PIN)) * FACTOR) - LOAD_CURRENT_ZERO) * LOAD_CURRENT_FACTOR;
        #endif
        energy[LOAD] += voltage[LOAD] * current[LOAD] * (SENSOR_PERIOD_MILLIS + millis() - nextRead) / 1000.0;

        #ifdef PANEL_VOLTAGE_PIN
            voltage[PANEL] = float(analogRead(PANEL_VOLTAGE_PIN)) * FACTOR;
        #endif
        #ifdef PANEL_CURRENT_PIN
            current[PANEL] = float(analogRead(PANEL_CURRENT_PIN)) * FACTOR;
        #endif
        energy[PANEL] += voltage[PANEL] * current[PANEL] * (SENSOR_PERIOD_MILLIS + millis() - nextRead) / 1000.0;

        nextRead += SENSOR_PERIOD_MILLIS;
    }
}

float SensorManager::getPower(int sensor){
    return voltage[sensor] * current[sensor];
}

float SensorManager::getVoltage(int sensor){
    return voltage[sensor];
}

float SensorManager::getCurrent(int sensor){
    return current[sensor];
}

float SensorManager::getEnergy(int sensor){
    return energy[sensor];
}


