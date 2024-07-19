#include <Arduino.h>
#include <EEPROM.h>

#include "store.h"

#define EEPROM_ADDR_RESTARTS 0
#define EEPROM_ADDR_LAST_UPTIME 1
#define EEPROM_ADDR_PANEL 10
#define EEPROM_ADDR_LOAD 12

Store::Store() :
    restarts(0),
    lastUptime(),
    nextPersist(0),
    writes(0)
{}

void Store::setup(){
    restarts = EEPROM.read(EEPROM_ADDR_RESTARTS);
    EEPROM.write(EEPROM_ADDR_RESTARTS, restarts + 1);

    lastUptime = readUnsignedInt(EEPROM_ADDR_LAST_UPTIME);

};

void Store::refresh(float panelEnergy, float loadEnergy){
    // It's very important we don't try and write to the EEPROM too often
    // as it has a limited number of write cycles, on the order of 100,000.
    if (millis() > nextPersist){
        // Serial.print("\n");
        // Serial.print("Store saving: ");
        // // Serial.print("panelEnergy: ");
        // // Serial.print(panelEnergy);
        // // Serial.print("\n");
        // // Serial.print("loadEnergy: ");
        // Serial.print(loadEnergy);
        // Serial.print("\n");

        nextPersist = millis() + 600000; // 10 Minutes
        writes += 1;
        writeUnsignedInt(EEPROM_ADDR_LAST_UPTIME, millis() / 1000 / 60);
        // Storing in watt hours:
        writeUnsignedInt(EEPROM_ADDR_PANEL, panelEnergy / 3600);
        writeUnsignedInt(EEPROM_ADDR_LOAD, loadEnergy / 3600);

        // Serial.print("Store checking saved: ");
        // // Serial.print("panelEnergy: ");
        // // Serial.print(getSavedEnergyPanel());
        // // Serial.print("\n");
        // // Serial.print("loadEnergy: ");
        // Serial.print(readUnsignedInt(EEPROM_ADDR_LOAD) * 36000);
        // Serial.print("\n");
    }

}

byte Store::getRestarts(){
    return restarts;
}

unsigned int Store::getLastUptime() {
    return lastUptime;
}

float Store::getSavedEnergyPanel() {
    return readUnsignedInt(EEPROM_ADDR_PANEL) * 3600.0;
    // return 0; // temp
}

float Store::getSavedEnergyLoad() {
    return readUnsignedInt(EEPROM_ADDR_LOAD) * 3600.0;
    // return 208.0 * 3600.0; // temp
}

void Store::writeUnsignedInt(byte addr, unsigned int value) {
    EEPROM.write(addr, value / 256);
    EEPROM.write(addr+1, value % 256);
}

unsigned int Store::readUnsignedInt(byte addr) {
    return EEPROM.read(addr)*256 + EEPROM.read(addr+1);
}

void Store::persistNow() {
    nextPersist = millis() - 1;
}