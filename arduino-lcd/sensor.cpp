#include "sensor.h"

#define FACTOR SENSOR_ADC_FACTOR
#define BAT_THEORETICAL_CAPACITY BAT_THEORETICAL_CAPACITY_WH * 3600.0

#define THROW1AVG5(x) (float((x * 0) + x + x + x + x + x) / 5.0)
    // Throw away the first reading and average the next five.
    // This is to try and improve stability.

#define ADC_READING(pin) (THROW1AVG5(analogRead(pin)) * FACTOR)
#define ADC_READING(pin) (THROW1AVG5(analogRead(pin)) * FACTOR)

SensorManager::SensorManager() :
    voltage_factor(),
    current_zero(),
    current_factor(),
    voltage_buffer(),
    current_buffer(),
    buffer_index(0),
    energy(),
    lastReadTime(),
    nextReadTime(0),
    min_bat(0),
    max_bat(0)
{}

void SensorManager::setup(float panelEnergy, float loadEnergy){
    energy[PANEL] = panelEnergy;
    energy[LOAD] = loadEnergy;

    voltage_factor[LOAD] = LOAD_VOLTAGE_FACTOR;
    voltage_factor[PANEL] = PANEL_VOLTAGE_FACTOR;
    voltage_factor[LOAD2] = LOAD2_VOLTAGE_FACTOR;

    current_zero[LOAD] = LOAD_CURRENT_ZERO;
    current_zero[PANEL] = PANEL_CURRENT_ZERO;
    current_zero[LOAD2] = LOAD2_CURRENT_ZERO;

    current_factor[LOAD] = LOAD_CURRENT_FACTOR;
    current_factor[PANEL] = PANEL_CURRENT_FACTOR;
    current_factor[LOAD2] = LOAD2_CURRENT_FACTOR;
}

void SensorManager::refresh(){
    if (millis() > nextReadTime){
        #ifdef LOAD_VOLTAGE_PIN
            voltage_buffer[LOAD][buffer_index] = ADC_READING(LOAD_VOLTAGE_PIN);
        #endif
        #ifdef LOAD_CURRENT_PIN
            current_buffer[LOAD][buffer_index] = ADC_READING(LOAD_CURRENT_PIN);
        #endif
        #if defined(LOAD_VOLTAGE_PIN) && defined(LOAD_CURRENT_PIN)
        energy[LOAD] += (
            readingToVoltage(LOAD, voltage_buffer[LOAD][buffer_index])
            * readingToCurrent(LOAD, current_buffer[LOAD][buffer_index])
            * (millis() - lastReadTime[LOAD]) / 1000.0
            );
        lastReadTime[LOAD] = millis();
        #endif

        #ifdef PANEL_VOLTAGE_PIN
            voltage_buffer[PANEL][buffer_index] = ADC_READING(PANEL_VOLTAGE_PIN);
        #endif
        #ifdef PANEL_CURRENT_PIN
            current_buffer[PANEL][buffer_index] = ADC_READING(PANEL_CURRENT_PIN);
        #endif
        #if defined(PANEL_VOLTAGE_PIN) && defined(PANEL_CURRENT_PIN)
        energy[PANEL] += (
            readingToVoltage(PANEL, voltage_buffer[PANEL][buffer_index])
            * readingToCurrent(PANEL, current_buffer[PANEL][buffer_index])
            * (millis() - lastReadTime[PANEL]) / 1000.0
            );
        lastReadTime[PANEL] = millis();
        #endif

        #ifdef LOAD2_VOLTAGE_PIN
            voltage_buffer[LOAD2][buffer_index] = ADC_READING(LOAD2_VOLTAGE_PIN);
        #endif
        #ifdef LOAD2_CURRENT_PIN
            current_buffer[LOAD2][buffer_index] = ADC_READING(LOAD2_CURRENT_PIN);
        #endif
        #if defined(LOAD2_VOLTAGE_PIN) && defined(LOAD2_CURRENT_PIN)
        energy[LOAD2] += (
            readingToVoltage(LOAD2, voltage_buffer[LOAD2][buffer_index])
            * readingToCurrent(LOAD2, current_buffer[LOAD2][buffer_index])
            * (millis() - lastReadTime[LOAD2]) / 1000.0
            );
        lastReadTime[LOAD2] = millis();
        #endif

        // Updating min_bat and max_bat:
        if (getNetEnergy() > max_bat){
            max_bat = getNetEnergy();
        }
        if (getNetEnergy() < min_bat){
            min_bat = getNetEnergy();
        }

        nextReadTime += SENSOR_PERIOD_MILLIS;
        buffer_index += 1;
        if (buffer_index >= SENSOR_READINGS_WINDOW){
            buffer_index = 0;
        }

        #ifdef LOG_READINGS
            #ifdef LOAD_VOLTAGE_PIN
            Serial.print(readingToVoltage(LOAD, voltage_buffer[LOAD][buffer_index]), 2);
            Serial.print(", ");
            #endif
            #ifdef LOAD2_VOLTAGE_PIN
            Serial.print(readingToVoltage(LOAD2, voltage_buffer[LOAD2][buffer_index]), 2);
            Serial.print(", ");
            #endif
            #ifdef PANEL_VOLTAGE_PIN
            Serial.print(readingToVoltage(PANEL, voltage_buffer[PANEL][buffer_index]), 2);
            Serial.print(", ");
            #endif
            #ifdef LOAD_CURRENT_PIN
            Serial.print(readingtoCurrent(LOAD, current_buffer[LOAD][buffer_index]), 2);
            Serial.print(", ");
            #endif
            #ifdef LOAD2_CURRENT_PIN
            Serial.print(readingtoCurrent(LOAD2, current_buffer[LOAD2][buffer_index]), 2);
            Serial.print(", ");
            #endif
            #ifdef PANEL_CURRENT_PIN
            Serial.print(readingtoCurrent(PANEL, current_buffer[PANEL][buffer_index]), 2);
            Serial.print(", ");
            #endif
            #if defined(LOAD_VOLTAGE_PIN) && defined(LOAD_CURRENT_PIN)
            Serial.print(readingToVoltage(LOAD, voltage_buffer[LOAD][buffer_index])
                * readingtoCurrent(LOAD, current_buffer[LOAD][buffer_index]), 2);
            Serial.print(", ");
            #endif
            #if defined(LOAD2_VOLTAGE_PIN) && defined(LOAD2_CURRENT_PIN)
            Serial.print(readingToVoltage(LOAD2, voltage_buffer[LOAD2][buffer_index])
                * readingtoCurrent(LOAD2, current_buffer[LOAD2][buffer_index]), 2);
            Serial.print(", ");
            #endif
            #if defined(PANEL_VOLTAGE_PIN) && defined(PANEL_CURRENT_PIN)
            Serial.print(readingToVoltage(PANEL, voltage_buffer[PANEL][buffer_index])
                * readingtoCurrent(PANEL, current_buffer[PANEL][buffer_index]), 2);
            Serial.print("\n");
            #endif
        #endif

    }
}

float SensorManager::readingToVoltage(int sensor, float reading){
    return reading * voltage_factor[sensor];
}

float SensorManager::readingToCurrent(int sensor, float reading, float zero, float factor){
    if (isnan(zero)) {
        zero = current_zero[sensor];
    }
    if (isnan(factor)) {
        factor = current_factor[sensor];
    }
    return (reading - zero) * factor;
}

float SensorManager::getPower(int sensor){
    float total = 0;
    for (unsigned int i=0; i<SENSOR_READINGS_WINDOW; ++i){
        total += readingToVoltage(sensor, voltage_buffer[sensor][i]) * readingToCurrent(sensor, current_buffer[sensor][i]);
    }
    return total / SENSOR_READINGS_WINDOW;
}

float SensorManager::getVoltage(int sensor){
    float total = 0;
    for (unsigned int i=0; i<SENSOR_READINGS_WINDOW; ++i){
        total += voltage_buffer[sensor][i];
    }
    return readingToVoltage(sensor, total) / SENSOR_READINGS_WINDOW;
}

float SensorManager::getCurrent(int sensor, float zero, float factor){
    float total = 0;
    for (unsigned int i=0; i<SENSOR_READINGS_WINDOW; ++i){
        total += current_buffer[sensor][i];
    }
    return readingToCurrent(sensor, total / SENSOR_READINGS_WINDOW, zero, factor);
}

float SensorManager::getEnergy(int sensor){
    return energy[sensor];
}

float SensorManager::getDuty(int sensor){
    /*
    Average power divided by maximum power.
    */
    float total_power = 0;
    // Accumulating several maximum powers so we can throw out anomalies (think: ~ 99 percentile):
    float max_power1 = 0;
    float max_power2 = 0;
    float max_power3 = 0;
    float max_power4 = 0;
    float max_power5 = 0;
    float max_power6 = 0;

    float power;

    for (unsigned int i=0; i<SENSOR_READINGS_WINDOW; ++i){
        power = readingToVoltage(sensor, voltage_buffer[sensor][i]) * readingToCurrent(sensor, current_buffer[sensor][i]);
        total_power += power;
        if (power >= max_power1){
            max_power6 = max_power5;
            max_power5 = max_power4;
            max_power4 = max_power3;
            max_power3 = max_power2;
            max_power2 = max_power1;
            max_power1 = power;
        } else if (power >= max_power2){
            max_power6 = max_power5;
            max_power5 = max_power4;
            max_power4 = max_power3;
            max_power3 = max_power2;
            max_power2 = power;
        } else if (power >= max_power3){
            max_power6 = max_power5;
            max_power5 = max_power4;
            max_power4 = max_power3;
            max_power3 = power;
        } else if (power >= max_power4){
            max_power6 = max_power5;
            max_power5 = max_power4;
            max_power4 = power;
        } else if (power >= max_power5){
            max_power6 = max_power5;
            max_power5 = power;
        } else if (power >= max_power6){
            max_power6 = power;
        }
    }

    if (SENSOR_READINGS_WINDOW >= 500){
        power = max_power6;
    } else if (SENSOR_READINGS_WINDOW >= 400){
        power = max_power5;
    } else if (SENSOR_READINGS_WINDOW >= 300){
        power = max_power4;
    } else if (SENSOR_READINGS_WINDOW >= 200){
        power = max_power3;
    } else if (SENSOR_READINGS_WINDOW >= 100){
        power = max_power2;
    } else {
        power = max_power1;
    }

    //
    if (abs(power) < 1){ // If our average power is too small to reasonably measure duty, assume 100%.
        return 1.0;
    }

    return (abs(total_power) / SENSOR_READINGS_WINDOW) / abs(power);
}


float SensorManager::getNetEnergy(){
    return (energy[PANEL] - (energy[LOAD] + energy[LOAD2])) * BAT_NET_EFFICIENCY;
}

float SensorManager::getBatLevel(){
    // We assume we're at 100% full batteries when turned on. So getNetEnergy() should be negative.
    // If max_bat is above 0, that means we weren't actully fully charged: put some energy into
    // the batteries after startup, so we account for that: treat the new level as 100%.
    return BAT_THEORETICAL_CAPACITY + (getNetEnergy() - max_bat);
}

float SensorManager::getBatCapacity(){
    return BAT_THEORETICAL_CAPACITY;
}

float SensorManager::getBatMin(){
    return min_bat;
}

float SensorManager::getBatMax(){
    return max_bat;
}

float SensorManager::getCurrentZero(int sensor){
    return current_zero[sensor];
}
void SensorManager::setCurrentZero(int sensor, float value){
    current_zero[sensor] = value;
}
float SensorManager::getCurrentFactor(int sensor){
    return current_factor[sensor];
}
void SensorManager::setCurrentFactor(int sensor, float value){
    current_factor[sensor] = value;
}
float SensorManager::getVoltageFactor(int sensor){
    return voltage_factor[sensor];
}
void SensorManager::setVoltageFactor(int sensor, float value){
    voltage_factor[sensor] = value;
}
