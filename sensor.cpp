#include "sensor.h"

#define FACTOR SENSOR_ADC_FACTOR

#define THROW1AVG5(x) (float((x * 0) + x + x + x + x + x) / 5.0)
    // Throw away the first reading and average the next five.
    // This is to try and improve stability.

#define VOLTAGE_READING(pin, factor) (THROW1AVG5(analogRead(pin)) * FACTOR * factor)
#define CURRENT_READING(pin, zero, factor, offset) ((((THROW1AVG5(analogRead(pin)) * FACTOR) - zero) * factor) + offset)

SensorManager::SensorManager() : voltage(), current(), energy(), nextReadTime(0), currentOffset() {}

void SensorManager::setup(){
}

void SensorManager::refresh(){
    if (millis() > nextReadTime){
        #ifdef LOAD_VOLTAGE_PIN
            voltage[LOAD] = VOLTAGE_READING(LOAD_VOLTAGE_PIN, LOAD_CURRENT_FACTOR);
        #endif
        #ifdef LOAD_CURRENT_PIN
            current[LOAD] = CURRENT_READING(LOAD_CURRENT_PIN, LOAD_CURRENT_ZERO, LOAD_CURRENT_FACTOR, currentOffset[LOAD]);
        #endif
        energy[LOAD] += voltage[LOAD] * current[LOAD] * (SENSOR_PERIOD_MILLIS + millis() - nextReadTime) / 1000.0;

        #ifdef PANEL_VOLTAGE_PIN
            voltage[PANEL] =VOLTAGE_READING(PANEL_VOLTAGE_PIN, PANEL_VOLTAGE_FACTOR);
        #endif
        #ifdef PANEL_CURRENT_PIN
            current[PANEL] = CURRENT_READING(PANEL_CURRENT_PIN, PANEL_CURRENT_ZERO, PANEL_CURRENT_FACTOR, currentOffset[PANEL]);
        #endif
        energy[PANEL] += voltage[PANEL] * current[PANEL] * (SENSOR_PERIOD_MILLIS + millis() - nextReadTime) / 1000.0;

        #ifdef LOAD2_VOLTAGE_PIN
            voltage[LOAD2] = VOLTAGE_READING(LOAD2_VOLTAGE_PIN, LOAD2_VOLTAGE_FACTOR);
        #endif
        #ifdef LOAD2_CURRENT_PIN
            current[LOAD2] = CURRENT_READING(LOAD2_CURRENT_PIN, LOAD2_CURRENT_ZERO, LOAD2_CURRENT_FACTOR, currentOffset[LOAD2]);
        #endif
        energy[LOAD2] += voltage[LOAD2] * current[LOAD2] * (SENSOR_PERIOD_MILLIS + millis() - nextReadTime) / 1000.0;


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


