#include "i2c_bus.h"

#if POWER_METER_HAS_STATUS_DISPLAY || POWER_METER_HAS_ADS1115

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#if !POWER_METER_HAS_STATUS_DISPLAY
#include <driver/i2c.h>
#endif

#ifndef POWER_METER_I2C_SDA
#define POWER_METER_I2C_SDA 5
#endif
#ifndef POWER_METER_I2C_SCL
#define POWER_METER_I2C_SCL 4
#endif
#ifndef POWER_METER_I2C_FREQUENCY
#define POWER_METER_I2C_FREQUENCY 400000
#endif
#ifndef POWER_METER_I2C_PORT
#define POWER_METER_I2C_PORT 1
#endif

namespace i2c_bus {
namespace {

SemaphoreHandle_t mutex = nullptr;
bool ready = false;
uint32_t errors = 0;
uint32_t lockTimeouts = 0;
uint32_t lastBeginAttemptMs = 0;
bool beginAttempted = false;
constexpr uint32_t kBeginRetryMs = 5000;

#if !POWER_METER_HAS_STATUS_DISPLAY
constexpr i2c_port_t kPort = static_cast<i2c_port_t>(POWER_METER_I2C_PORT);
#endif

bool writeBytes(uint8_t address, const uint8_t* bytes, size_t length) {
#if POWER_METER_HAS_STATUS_DISPLAY
    Wire.beginTransmission(address);
    Wire.write(bytes, length);
    return Wire.endTransmission() == 0;
#else
    return i2c_master_write_to_device(kPort, address, bytes, length,
                                      pdMS_TO_TICKS(50)) == ESP_OK;
#endif
}

bool writeRead(uint8_t address, const uint8_t* writeData, size_t writeLength,
               uint8_t* readData, size_t readLength) {
#if POWER_METER_HAS_STATUS_DISPLAY
    Wire.beginTransmission(address);
    Wire.write(writeData, writeLength);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom(address, static_cast<uint8_t>(readLength)) != readLength) return false;
    for (size_t i = 0; i < readLength; ++i) readData[i] = Wire.read();
    return true;
#else
    return i2c_master_write_read_device(kPort, address, writeData, writeLength,
                                        readData, readLength, pdMS_TO_TICKS(50)) == ESP_OK;
#endif
}

} // namespace

bool begin() {
    if (ready) return true;
    beginAttempted = true;
    lastBeginAttemptMs = millis();
    if (!mutex) mutex = xSemaphoreCreateMutex();
    if (!mutex) {
        ++errors;
        Serial.println("i2c: failed to create mutex");
        return false;
    }
#if POWER_METER_HAS_STATUS_DISPLAY
    if (!Wire.begin(POWER_METER_I2C_SDA, POWER_METER_I2C_SCL, POWER_METER_I2C_FREQUENCY)) {
        ++errors;
        Serial.println("i2c: Wire.begin failed");
        return false;
    }
    Wire.setTimeOut(50);
    const char* backend = "arduino";
    const int backendPort = 0;
#else
    i2c_config_t config{};
    config.mode = I2C_MODE_MASTER;
    config.sda_io_num = static_cast<gpio_num_t>(POWER_METER_I2C_SDA);
    config.scl_io_num = static_cast<gpio_num_t>(POWER_METER_I2C_SCL);
    config.sda_pullup_en = GPIO_PULLUP_ENABLE;
    config.scl_pullup_en = GPIO_PULLUP_ENABLE;
    config.master.clk_speed = POWER_METER_I2C_FREQUENCY;
    if (i2c_param_config(kPort, &config) != ESP_OK ||
        i2c_driver_install(kPort, config.mode, 0, 0, 0) != ESP_OK) {
        ++errors;
        Serial.println("i2c: ESP-IDF driver initialization failed");
        return false;
    }
    const char* backend = "esp-idf";
    const int backendPort = POWER_METER_I2C_PORT;
#endif
    ready = true;
    Serial.printf("i2c: ready backend=%s port=%d sda=%d scl=%d frequency=%d\n",
                  backend, backendPort, POWER_METER_I2C_SDA,
                  POWER_METER_I2C_SCL, POWER_METER_I2C_FREQUENCY);
    return true;
}

void update() {
    if (ready) return;
    const uint32_t now = millis();
    if (beginAttempted && now - lastBeginAttemptMs < kBeginRetryMs) return;
    begin();
}

bool isReady() { return ready; }

#if POWER_METER_HAS_STATUS_DISPLAY
TwoWire& wire() { return Wire; }
#endif

Guard::Guard(uint32_t timeoutMs) {
    if (!ready || !mutex) return;
    locked_ = xSemaphoreTake(mutex, pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
    if (!locked_) {
        ++errors;
        ++lockTimeouts;
    }
}

Guard::~Guard() {
    if (locked_) xSemaphoreGive(mutex);
}

bool probe(uint8_t address) {
    Guard guard;
    if (!guard) return false;
#if POWER_METER_HAS_STATUS_DISPLAY
    Wire.beginTransmission(address);
    const bool found = Wire.endTransmission() == 0;
#else
    i2c_cmd_handle_t command = i2c_cmd_link_create();
    bool found = false;
    if (command) {
        i2c_master_start(command);
        i2c_master_write_byte(command, static_cast<uint8_t>(address << 1), true);
        i2c_master_stop(command);
        found = i2c_master_cmd_begin(kPort, command, pdMS_TO_TICKS(50)) == ESP_OK;
        i2c_cmd_link_delete(command);
    }
#endif
    if (!found) ++errors;
    return found;
}

bool writeRegister16(uint8_t address, uint8_t reg, uint16_t value) {
    Guard guard;
    if (!guard) return false;
    const uint8_t bytes[] = {reg, static_cast<uint8_t>(value >> 8),
                            static_cast<uint8_t>(value & 0xff)};
    const bool ok = writeBytes(address, bytes, sizeof(bytes));
    if (!ok) ++errors;
    return ok;
}

bool readRegister16(uint8_t address, uint8_t reg, uint16_t& value) {
    Guard guard;
    if (!guard) return false;
    uint8_t bytes[2]{};
    const bool ok = writeRead(address, &reg, 1, bytes, sizeof(bytes));
    if (!ok) {
        ++errors;
        return false;
    }
    value = static_cast<uint16_t>(bytes[0] << 8) | bytes[1];
    return true;
}

uint32_t errorCount() { return errors; }
uint32_t lockTimeoutCount() { return lockTimeouts; }

} // namespace i2c_bus

#endif
