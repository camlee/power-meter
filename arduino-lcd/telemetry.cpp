#include "telemetry.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "hardware.h"
#include "settings.h"

namespace {

const uint8_t IN_MASK = 0x01;
const uint8_t OUT_MASK = 0x02;
const uint8_t AUX_MASK = 0x04;
const float MAX_FORMATTABLE_MAGNITUDE = 999999.0;

bool formatValue(float value, uint8_t precision, char *destination, size_t capacity){
    if (!isfinite(value) || fabs(value) > MAX_FORMATTABLE_MAGNITUDE || capacity < 2){
        return false;
    }

    dtostrf(value, 0, precision, destination);
    return strlen(destination) < capacity;
}

bool formatChannel(SensorManager *sensors,
                   uint8_t sensor,
                   bool includeDuty,
                   char storage[3][12],
                   const char *fields[3]){
    if (!formatValue(sensors->getVoltage(sensor), 2, storage[0], sizeof(storage[0])) ||
        !formatValue(sensors->getCurrent(sensor), 2, storage[1], sizeof(storage[1]))){
        return false;
    }

    fields[0] = storage[0];
    fields[1] = storage[1];
    fields[2] = "";

    if (includeDuty){
        const float duty = sensors->getDuty(sensor);
        if (isfinite(duty) && duty >= 0.0 && duty <= 1.0 &&
            formatValue(duty, 3, storage[2], sizeof(storage[2]))){
            fields[2] = storage[2];
        }
    }

    return true;
}

} // namespace

TelemetryTransmitter::TelemetryTransmitter(SensorManager *sensorManager) :
    sensorManager(sensorManager),
    pendingRecord(),
    pendingLength(0),
    pendingOffset(0),
    sequence(0),
    nextRecordTime(0)
{}

void TelemetryTransmitter::setup(){
    nextRecordTime = millis() + TELEMETRY_PERIOD_MILLIS;
}

void TelemetryTransmitter::refresh(){
    pumpSerial();

    const unsigned long now = millis();
    if (pendingOffset >= pendingLength &&
        static_cast<long>(now - nextRecordTime) >= 0){
        if (queueSnapshot(now)){
            ++sequence;
        }
        nextRecordTime = now + TELEMETRY_PERIOD_MILLIS;
        pumpSerial();
    }
}

bool TelemetryTransmitter::queueSnapshot(unsigned long snapshotMillis){
    char values[PM1_FIELD_COUNT][12] = {};
    const char *fields[PM1_FIELD_COUNT] = {"", "", "", "", "", "", "", "", ""};
    uint8_t mask = 0;

#ifdef PM1_IN_SENSOR
    if (!formatChannel(sensorManager,
                       PM1_IN_SENSOR,
                       PM1_IN_DUTY_AVAILABLE,
                       &values[0],
                       &fields[0])){
        return false;
    }
    mask |= IN_MASK;
#endif

#ifdef PM1_OUT_SENSOR
    if (!formatChannel(sensorManager,
                       PM1_OUT_SENSOR,
                       PM1_OUT_DUTY_AVAILABLE,
                       &values[3],
                       &fields[3])){
        return false;
    }
    mask |= OUT_MASK;
#endif

#ifdef PM1_AUX_SENSOR
    if (!formatChannel(sensorManager,
                       PM1_AUX_SENSOR,
                       PM1_AUX_DUTY_AVAILABLE,
                       &values[6],
                       &fields[6])){
        return false;
    }
    mask |= AUX_MASK;
#endif

    const size_t length = pm1FormatRecord(pendingRecord,
                                          sizeof(pendingRecord),
                                          sequence,
                                          snapshotMillis,
                                          mask,
                                          fields);
    if (length == 0 || length > 0xFF){
        return false;
    }

    pendingLength = static_cast<uint8_t>(length);
    pendingOffset = 0;
    return true;
}

void TelemetryTransmitter::pumpSerial(){
    if (pendingOffset >= pendingLength){
        return;
    }

    const int available = Serial.availableForWrite();
    if (available <= 0){
        return;
    }

    const uint8_t remaining = pendingLength - pendingOffset;
    const uint8_t count = remaining < available ? remaining : static_cast<uint8_t>(available);
    const size_t sent = Serial.write(
        reinterpret_cast<const uint8_t *>(pendingRecord + pendingOffset), count);
    pendingOffset += static_cast<uint8_t>(sent);
}
