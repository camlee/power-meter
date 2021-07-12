#define LED_PIN 13

// LCD Shield:
#define LIQUID_CRYSTAL_ARGS 8, 9, 4, 5, 6, 7
#define BACKLIGHT_PIN 10
#define BUTTONS_PIN 0

// Sensors:
#define LOAD_CURRENT_PIN 1
#define LOAD_VOLTAGE_PIN 2
#define PANEL_CURRENT_PIN 3
#define PANEL_VOLTAGE_PIN 4
// #define LOAD2_CURRENT_PIN 5
// #define LOAD2_VOLTAGE_PIN 2

#define SENSOR_ADC_MAX_VALUE 1024.0
#define SENSOR_ADC_REFERENCE_VOLTAGE 5.07

// Specific sensors:
#define LOAD_VOLTAGE_FACTOR 7.194
#define LOAD_CURRENT_ZERO 2.5277
#define LOAD_CURRENT_FACTOR 14.776

#define PANEL_VOLTAGE_FACTOR 7.297
#define PANEL_CURRENT_ZERO 2.525
#define PANEL_CURRENT_FACTOR 14.776

#define LOAD2_VOLTAGE_FACTOR 6.5277
#define LOAD2_CURRENT_ZERO 2.53
#define LOAD2_CURRENT_FACTOR 4.79

// Battery:
#define BAT_THEORETICAL_CAPACITY_WH 2500 // 215 AH at 5h, 260 AH at 20h (two 6V in series, 2021).


// Voltage factors:
// Long one: 7.2497
// 1: 6.5277
// 2: 7.0797

// Current zeros:
// Long one:
// 1: 2.53
// 2: 2.53

// Current factors:
// Long one:
// 1: 4.79
// 2: 14.68

#define SENSOR_ADC_FACTOR SENSOR_ADC_REFERENCE_VOLTAGE / SENSOR_ADC_MAX_VALUE