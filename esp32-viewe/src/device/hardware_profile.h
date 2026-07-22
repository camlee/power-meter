#pragma once

#ifndef POWER_METER_HARDWARE_PROFILE
#define POWER_METER_HARDWARE_PROFILE "meter-viewe"
#endif

#ifndef POWER_METER_HAS_TOUCH_UI
#define POWER_METER_HAS_TOUCH_UI 1
#endif

#ifndef POWER_METER_HAS_STATUS_DISPLAY
#define POWER_METER_HAS_STATUS_DISPLAY 0
#endif

#ifndef POWER_METER_HAS_ESP32_ADC
#define POWER_METER_HAS_ESP32_ADC 1
#endif

#ifndef POWER_METER_HAS_ADS1115
#define POWER_METER_HAS_ADS1115 0
#endif

#ifndef POWER_METER_SUPPORTS_UART
#define POWER_METER_SUPPORTS_UART 1
#endif

namespace hardware_profile {

constexpr const char* kName = POWER_METER_HARDWARE_PROFILE;
constexpr bool kHasTouchUi = POWER_METER_HAS_TOUCH_UI != 0;
constexpr bool kHasStatusDisplay = POWER_METER_HAS_STATUS_DISPLAY != 0;
constexpr bool kHasEsp32Adc = POWER_METER_HAS_ESP32_ADC != 0;
constexpr bool kHasAds1115 = POWER_METER_HAS_ADS1115 != 0;
constexpr bool kSupportsUart = POWER_METER_SUPPORTS_UART != 0;
constexpr bool kSupportsDemo = true;

} // namespace hardware_profile
