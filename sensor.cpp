#include "sensor.h"

SensorManager::SensorManager() : voltage(0.0), current(0.0){}

void SensorManager::setup(){

}

void SensorManager::refresh(){
    voltage = analogRead(LOAD_VOLTAGE_PIN) * 5 / 1023;
    current = analogRead(LOAD_CURRENT_PIN) * 5 / 1023;
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

