#ifndef POWER_METER_STORE_HEADER
#define POWER_METER_STORE_HEADER

#include "hardware.h"

class Store {
private:
    unsigned int restarts;
    unsigned long nextPersist;

    void writeUnsignedInt(byte, unsigned int);
    unsigned int readUnsignedInt(byte);

public:
    int writes;

    Store();
    void setup();
    void refresh();

    unsigned int getRestarts();

    float getSavedEnergyPanel();
    float getSavedEnergyLoad();
    void setSavedEnergy(float, float, bool=false);

    float getSavedPanelCurrentZero();
    void setSavedPanelCurrentZero(float);
    float getSavedPanelCurrentFactor();
    void setSavedPanelCurrentFactor(float);
    float getSavedPanelVoltageFactor();
    void setSavedPanelVoltageFactor(float);

    float getSavedLoadCurrentZero();
    void setSavedLoadCurrentZero(float);
    float getSavedLoadCurrentFactor();
    void setSavedLoadCurrentFactor(float);
    float getSavedLoadVoltageFactor();
    void setSavedLoadVoltageFactor(float);
};

#endif