#include "sensor.h"

#define FACTOR SENSOR_ADC_FACTOR
#define BAT_THEORETICAL_CAPACITY BAT_THEORETICAL_CAPACITY_WH * 3600.0

#define THROW1AVG5(x) (float((x * 0) + x + x + x + x + x) / 5.0)
    // Throw away the first reading and average the next five.
    // This is to try and improve stability.

#define VOLTAGE_READING(pin, factor) (THROW1AVG5(analogRead(pin)) * FACTOR * factor)
#define CURRENT_READING(pin, zero, factor) (((THROW1AVG5(analogRead(pin)) * FACTOR) - zero) * factor)

SensorManager::SensorManager() :
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
    // Serial.print("SensorManager setUp with: ");
    // Serial.print("panelEnergy: ");
    // Serial.print(panelEnergy);
    // Serial.print("\n");
    // Serial.print("loadEnergy: ");
    // Serial.print(loadEnergy);
    // Serial.print("\n");


    energy[PANEL] = panelEnergy;
    energy[LOAD] = loadEnergy;
}

void SensorManager::refresh(){
    if (millis() > nextReadTime){
        #ifdef LOAD_VOLTAGE_PIN
            voltage_buffer[LOAD][buffer_index] = VOLTAGE_READING(LOAD_VOLTAGE_PIN, LOAD_VOLTAGE_FACTOR);
        #endif
        #ifdef LOAD_CURRENT_PIN
            current_buffer[LOAD][buffer_index] = CURRENT_READING(LOAD_CURRENT_PIN, LOAD_CURRENT_ZERO, LOAD_CURRENT_FACTOR);
        #endif
        #if defined(LOAD_VOLTAGE_PIN) && defined(LOAD_CURRENT_PIN)
        energy[LOAD] += voltage_buffer[LOAD][buffer_index] * current_buffer[LOAD][buffer_index] * (millis() - lastReadTime[LOAD]) / 1000.0;
        lastReadTime[LOAD] = millis();
        #endif

        #ifdef PANEL_VOLTAGE_PIN
            voltage_buffer[PANEL][buffer_index] = VOLTAGE_READING(PANEL_VOLTAGE_PIN, PANEL_VOLTAGE_FACTOR);
        #endif
        #ifdef PANEL_CURRENT_PIN
            current_buffer[PANEL][buffer_index] = CURRENT_READING(PANEL_CURRENT_PIN, PANEL_CURRENT_ZERO, PANEL_CURRENT_FACTOR);
        #endif
        #if defined(PANEL_VOLTAGE_PIN) && defined(PANEL_CURRENT_PIN)
        energy[PANEL] += voltage_buffer[PANEL][buffer_index] * current_buffer[PANEL][buffer_index] * (millis() - lastReadTime[PANEL]) / 1000.0;
        lastReadTime[PANEL] = millis();
        #endif

        #ifdef LOAD2_VOLTAGE_PIN
            voltage_buffer[LOAD2][buffer_index] = VOLTAGE_READING(LOAD2_VOLTAGE_PIN, LOAD2_VOLTAGE_FACTOR);
        #endif
        #ifdef LOAD2_CURRENT_PIN
            current_buffer[LOAD2][buffer_index] = CURRENT_READING(LOAD2_CURRENT_PIN, LOAD2_CURRENT_ZERO, LOAD2_CURRENT_FACTOR);
        #endif
        #if defined(LOAD2_VOLTAGE_PIN) && defined(LOAD2_CURRENT_PIN)
        energy[LOAD2] += voltage_buffer[LOAD2][buffer_index] * current_buffer[LOAD2][buffer_index] * (millis() - lastReadTime[LOAD2]) / 1000.0;
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
            Serial.print(voltage_buffer[LOAD][buffer_index], 2);
            Serial.print(", ");
            #endif
            #ifdef LOAD2_VOLTAGE_PIN
            Serial.print(voltage_buffer[LOAD2][buffer_index], 2);
            Serial.print(", ");
            #endif
            #ifdef PANEL_VOLTAGE_PIN
            Serial.print(voltage_buffer[PANEL][buffer_index], 2);
            Serial.print(", ");
            #endif
            #ifdef LOAD_CURRENT_PIN
            Serial.print(current_buffer[LOAD][buffer_index], 2);
            Serial.print(", ");
            #endif
            #ifdef LOAD2_CURRENT_PIN
            Serial.print(current_buffer[LOAD2][buffer_index], 2);
            Serial.print(", ");
            #endif
            #ifdef PANEL_CURRENT_PIN
            Serial.print(current_buffer[PANEL][buffer_index], 2);
            Serial.print(", ");
            #endif
            #if defined(LOAD_VOLTAGE_PIN) && defined(LOAD_CURRENT_PIN)
            Serial.print(voltage_buffer[LOAD][buffer_index] * current_buffer[LOAD][buffer_index], 2);
            Serial.print(", ");
            #endif
            #if defined(LOAD2_VOLTAGE_PIN) && defined(LOAD2_CURRENT_PIN)
            Serial.print(voltage_buffer[LOAD2][buffer_index] * current_buffer[LOAD2][buffer_index], 2);
            Serial.print(", ");
            #endif
            #if defined(PANEL_VOLTAGE_PIN) && defined(PANEL_CURRENT_PIN)
            Serial.print(voltage_buffer[PANEL][buffer_index] * current_buffer[PANEL][buffer_index], 2);
            Serial.print("\n");
            #endif
        #endif

    }
}


float SensorManager::getPower(int sensor){
    float total = 0;
    for (unsigned int i=0; i<SENSOR_READINGS_WINDOW; ++i){
        total += voltage_buffer[sensor][i] * current_buffer[sensor][i];
    }
    return total / SENSOR_READINGS_WINDOW;
}

float SensorManager::getVoltage(int sensor){
    float total = 0;
    for (unsigned int i=0; i<SENSOR_READINGS_WINDOW; ++i){
        total += voltage_buffer[sensor][i];
    }
    return total / SENSOR_READINGS_WINDOW;
}

float SensorManager::getCurrent(int sensor){
    float total = 0;
    for (unsigned int i=0; i<SENSOR_READINGS_WINDOW; ++i){
        total += current_buffer[sensor][i];
    }
    return total / SENSOR_READINGS_WINDOW;
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
        total_power += voltage_buffer[sensor][i] * current_buffer[sensor][i];
        power = voltage_buffer[sensor][i] * current_buffer[sensor][i];
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