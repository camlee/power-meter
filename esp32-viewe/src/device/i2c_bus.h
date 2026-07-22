#pragma once

#include <cstddef>
#include <cstdint>

#if POWER_METER_HAS_STATUS_DISPLAY || POWER_METER_HAS_ADS1115

#if POWER_METER_HAS_STATUS_DISPLAY
#include <Wire.h>
#endif

namespace i2c_bus {

bool begin();
void update();
bool isReady();
bool probe(uint8_t address);
uint32_t errorCount();
uint32_t lockTimeoutCount();
bool writeRegister16(uint8_t address, uint8_t reg, uint16_t value);
bool readRegister16(uint8_t address, uint8_t reg, uint16_t& value);

#if POWER_METER_HAS_STATUS_DISPLAY
// Adafruit_SSD1306 requires Arduino Wire. Targets without the status display
// use the ESP-IDF legacy backend instead, so they can coexist with VIEWE's
// display-panel I2C driver.
TwoWire& wire();
#endif

class Guard {
public:
    explicit Guard(uint32_t timeoutMs = 50);
    ~Guard();
    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;
    explicit operator bool() const { return locked_; }

private:
    bool locked_ = false;
};

} // namespace i2c_bus

#else

namespace i2c_bus {
inline bool begin() { return false; }
inline void update() {}
inline bool isReady() { return false; }
inline bool probe(uint8_t) { return false; }
inline uint32_t errorCount() { return 0; }
inline uint32_t lockTimeoutCount() { return 0; }
inline bool writeRegister16(uint8_t, uint8_t, uint16_t) { return false; }
inline bool readRegister16(uint8_t, uint8_t, uint16_t&) { return false; }
} // namespace i2c_bus

#endif
