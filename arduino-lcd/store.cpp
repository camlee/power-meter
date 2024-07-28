#include <Arduino.h>
#include <EEPROM.h>

#include "store.h"

#define EEPROM_ADDR_RESTARTS 0
#define EEPROM_ADDR_PANEL_ENERGY 10
#define EEPROM_ADDR_LOAD_ENERGY 12

#define EEPROM_ADDR_CALIBRATION_PANEL_CURRENT_ZERO 20
#define EEPROM_ADDR_CALIBRATION_PANEL_CURRENT_FACTOR 22
#define EEPROM_ADDR_CALIBRATION_PANEL_VOLTAGE_FACTOR 24
#define EEPROM_ADDR_CALIBRATION_LOAD_CURRENT_ZERO 26
#define EEPROM_ADDR_CALIBRATION_LOAD_CURRENT_FACTOR 28
#define EEPROM_ADDR_CALIBRATION_LOAD_VOLTAGE_FACTOR 30


Store::Store() :
    restarts(),
    nextPersist(0),
    writes(0)
{}

void Store::setup(){
    restarts = readUnsignedInt(EEPROM_ADDR_RESTARTS);
    writeUnsignedInt(EEPROM_ADDR_RESTARTS, restarts+1);
};

void Store::refresh(){}

unsigned int Store::getRestarts(){
    return restarts;
}

float Store::getSavedEnergyPanel() {
    return readUnsignedInt(EEPROM_ADDR_PANEL_ENERGY) * 3600.0;
    // return 0 * 3600.0; // temp
}

float Store::getSavedEnergyLoad() {
    return readUnsignedInt(EEPROM_ADDR_LOAD_ENERGY) * 3600.0;
    // return 185.0 * 3600.0; // temp
}

void Store::setSavedEnergy(float panelEnergy, float loadEnergy, bool persistNow) {
    // It's very important we don't try and write to the EEPROM too often
    // as it has a limited number of write cycles, on the order of 100,000.
    if (persistNow || millis() > nextPersist){
        nextPersist = millis() + 600000; // 10 Minutes

        // Storing in watt hours:
        writeUnsignedInt(EEPROM_ADDR_PANEL_ENERGY, panelEnergy / 3600);
        writeUnsignedInt(EEPROM_ADDR_LOAD_ENERGY, loadEnergy / 3600);
    }
}


float Store::getSavedPanelCurrentZero(){
    unsigned int value = readUnsignedInt(EEPROM_ADDR_CALIBRATION_PANEL_CURRENT_ZERO);
    if (value == 65535 || value == 0){ // Nothing saved to EEPROM yet
        return PANEL_CURRENT_ZERO;
    }
    // Serial.print("Using saved panel current zero of ");
    // Serial.print(value);
    // Serial.print("\n");
    return (float)value / 1000.0;
}
void Store::setSavedPanelCurrentZero(float value){
    writeUnsignedInt(EEPROM_ADDR_CALIBRATION_PANEL_CURRENT_ZERO, (unsigned int)(value * 1000.0));
}
float Store::getSavedPanelCurrentFactor(){
    unsigned int value = readUnsignedInt(EEPROM_ADDR_CALIBRATION_PANEL_CURRENT_FACTOR);
    if (value == 65535 || value == 0){ // Nothing saved to EEPROM yet
        return PANEL_CURRENT_FACTOR;
    }
    // Serial.print("Using saved panel current factor of ");
    // Serial.print(value);
    // Serial.print("\n");
    return (float)value / 1000.0;
}
void Store::setSavedPanelCurrentFactor(float value){
    writeUnsignedInt(EEPROM_ADDR_CALIBRATION_PANEL_CURRENT_FACTOR, (unsigned int)(value * 1000.0));
}
float Store::getSavedPanelVoltageFactor(){
    unsigned int value = readUnsignedInt(EEPROM_ADDR_CALIBRATION_PANEL_VOLTAGE_FACTOR);
    if (value == 65535 || value == 0){ // Nothing saved to EEPROM yet
        return PANEL_VOLTAGE_FACTOR;
    }
    // Serial.print("Using saved panel voltage factor of ");
    // Serial.print(value);
    // Serial.print("\n");
    return (float)value / 1000.0;
}
void Store::setSavedPanelVoltageFactor(float value){
    writeUnsignedInt(EEPROM_ADDR_CALIBRATION_PANEL_VOLTAGE_FACTOR, (unsigned int)(value * 1000.0));
}

float Store::getSavedLoadCurrentZero(){
    unsigned int value = readUnsignedInt(EEPROM_ADDR_CALIBRATION_LOAD_CURRENT_ZERO);
    if (value == 65535 || value == 0){ // Nothing saved to EEPROM yet
        return LOAD_CURRENT_ZERO;
    }
    // Serial.print("Using saved load zero of ");
    // Serial.print(value);
    // Serial.print("\n");
    return (float)value / 1000.0;
}
void Store::setSavedLoadCurrentZero(float value){
    writeUnsignedInt(EEPROM_ADDR_CALIBRATION_LOAD_CURRENT_ZERO, (unsigned int)(value * 1000.0));
}
float Store::getSavedLoadCurrentFactor(){
    unsigned int value = readUnsignedInt(EEPROM_ADDR_CALIBRATION_LOAD_CURRENT_FACTOR);
    if (value == 65535 || value == 0){ // Nothing saved to EEPROM yet
        return LOAD_CURRENT_FACTOR;
    }
    // Serial.print("Using saved load current factor of ");
    // Serial.print(value);
    // Serial.print("\n");
    return (float)value / 1000.0;
}
void Store::setSavedLoadCurrentFactor(float value){
    writeUnsignedInt(EEPROM_ADDR_CALIBRATION_LOAD_CURRENT_FACTOR, (unsigned int)(value * 1000.0));
}
float Store::getSavedLoadVoltageFactor(){
    unsigned int value = readUnsignedInt(EEPROM_ADDR_CALIBRATION_LOAD_VOLTAGE_FACTOR);
    if (value == 65535 || value == 0){ // Nothing saved to EEPROM yet
        return LOAD_VOLTAGE_FACTOR;
    }
    // Serial.print("Using saved load voltage factor of ");
    // Serial.print(value);
    // Serial.print("\n");
    return (float)value / 1000.0;
}
void Store::setSavedLoadVoltageFactor(float value){
    writeUnsignedInt(EEPROM_ADDR_CALIBRATION_LOAD_VOLTAGE_FACTOR, (unsigned int)(value * 1000.0));
}



void Store::writeUnsignedInt(byte addr, unsigned int value) {
    writes += 2;
    EEPROM.write(addr, value / 256);
    EEPROM.write(addr+1, value % 256);
}

unsigned int Store::readUnsignedInt(byte addr) {
    unsigned int value = ((unsigned int) EEPROM.read(addr))*256 + EEPROM.read(addr+1);
    return value;
}