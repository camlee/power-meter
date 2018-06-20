#include "sensor.h"

SensorManager::SensorManager() : voltage(0.0), current(0.0), energy(0.0), nextRead(0){}

void SensorManager::setup(){

}

void SensorManager::refresh(){
    if (millis() > nextRead){
        voltage = analogRead(LOAD_VOLTAGE_PIN) * 5 / 1023;
        current = analogRead(LOAD_CURRENT_PIN) * 5 / 1023;
        energy += voltage * current * (SENSOR_PERIOD_MILLIS + millis() - nextRead) / 1000.0;

        nextRead = millis() + SENSOR_PERIOD_MILLIS;
    }
}

float SensorManager::getPower(){
    return voltage * current;
}

float SensorManager::getVoltage(){
    return voltage;
}

float SensorManager::getCurrent(){
    return current;
}

float SensorManager::getEnergy(){
    return energy;
}


