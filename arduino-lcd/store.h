#include "hardware.h"


class Store {
private:
    byte restarts;
    unsigned int lastUptime;
    unsigned long nextPersist;

    void writeUnsignedInt(byte, unsigned int);
    unsigned int readUnsignedInt(byte);

public:
    int writes;

    Store();
    void setup();
    void refresh(float, float);

    byte getRestarts();
    unsigned int getLastUptime(); // In minutes

    float getSavedEnergyPanel();
    float getSavedEnergyLoad();

    void persistNow();
};