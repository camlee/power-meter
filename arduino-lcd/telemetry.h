#ifndef POWER_METER_TELEMETRY_HEADER
#define POWER_METER_TELEMETRY_HEADER

#include <Arduino.h>

#include "pm1_protocol.h"
#include "sensor.h"

class TelemetryTransmitter {
private:
    SensorManager *sensorManager;
    char pendingRecord[PM1_MAX_RECORD_LENGTH];
    uint8_t pendingLength;
    uint8_t pendingOffset;
    uint32_t sequence;
    unsigned long nextRecordTime;

    bool queueSnapshot(unsigned long);
    void pumpSerial();

public:
    explicit TelemetryTransmitter(SensorManager *);
    void setup();
    void refresh();
};

#endif
