#include "sensor.h"

#define FACTOR SENSOR_ADC_FACTOR

SensorManager::SensorManager() : voltage(), current(), energy(), nextReadTime(0), currentOffset() {}

void SensorManager::setup(){
}

void SensorManager::refresh(){
    if (millis() > nextReadTime){
        #ifdef LOAD_VOLTAGE_PIN
            voltage[LOAD] = float(analogRead(LOAD_VOLTAGE_PIN)) * FACTOR * LOAD_VOLTAGE_FACTOR;
        #endif
        #ifdef LOAD_CURRENT_PIN
            current[LOAD] = (((float(analogRead(LOAD_CURRENT_PIN)) * FACTOR)
                - LOAD_CURRENT_ZERO) * LOAD_CURRENT_FACTOR) + currentOffset[LOAD];
        #endif
        energy[LOAD] += voltage[LOAD] * current[LOAD]
                * (SENSOR_PERIOD_MILLIS + millis() - nextReadTime) / 1000.0;

        #ifdef PANEL_VOLTAGE_PIN
            voltage[PANEL] = float(analogRead(PANEL_VOLTAGE_PIN)) * FACTOR * PANEL_VOLTAGE_FACTOR;
        #endif
        #ifdef PANEL_CURRENT_PIN
            current[PANEL] = (((float(analogRead(PANEL_CURRENT_PIN)) * FACTOR)
                - PANEL_CURRENT_ZERO) * PANEL_CURRENT_FACTOR) + currentOffset[PANEL];
        #endif
        energy[PANEL] += voltage[PANEL] * current[PANEL]
                * (SENSOR_PERIOD_MILLIS + millis() - nextReadTime) / 1000.0;

        #ifdef LOAD2_VOLTAGE_PIN
            voltage[LOAD2] = float(analogRead(LOAD2_VOLTAGE_PIN)) * FACTOR * LOAD2_VOLTAGE_FACTOR;
        #endif
        #ifdef LOAD2_CURRENT_PIN
            current[LOAD2] = (((float(analogRead(LOAD2_CURRENT_PIN)) * FACTOR)
                - LOAD2_CURRENT_ZERO) * LOAD2_CURRENT_FACTOR) + currentOffset[LOAD2];
        #endif
        energy[LOAD2] += voltage[LOAD2] * current[LOAD2]
                * (SENSOR_PERIOD_MILLIS + millis() - nextReadTime) / 1000.0;


        nextReadTime += SENSOR_PERIOD_MILLIS;

        #ifdef LOG_READINGS
            Serial.print(getVoltage(LOAD), 2);
            Serial.print(", ");
            Serial.print(getVoltage(LOAD2), 2);
            Serial.print(", ");
            Serial.print(getVoltage(PANEL), 2);
            Serial.print(", ");
            Serial.print(getCurrent(LOAD), 2);
            Serial.print(", ");
            Serial.print(getCurrent(LOAD2), 2);
            Serial.print(", ");
            Serial.print(getCurrent(PANEL), 2);
            Serial.print(", ");
            Serial.print(getPower(LOAD), 2);
            Serial.print(", ");
            Serial.print(getPower(LOAD2), 2);
            Serial.print(", ");
            Serial.print(getPower(PANEL), 2);
            Serial.print("\n");
        #endif

    }
}

void SensorManager::zeroCurrent(int sensor){
    currentOffset[sensor] -= getCurrent(sensor);
}

float SensorManager::getPower(int sensor){
    return getCurrent(sensor) * getVoltage(sensor);
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


