#define LED_PIN 13

// LCD Shield:
#define LIQUID_CRYSTAL_ARGS 8, 9, 4, 5, 6, 7
#define BACKLIGHT_PIN 10
#define BUTTONS_PIN 0

// Sensors:
#define SENSOR_PERIOD_MILLIS 100

#define LOAD_CURRENT_PIN 1
#define LOAD_VOLTAGE_PIN 2
#define PANEL_CURRENT_PIN 3
#define PANEL_VOLTAGE_PIN 4

#define SENSOR_ADC_MAX_VALUE 1024.0
#define SENSOR_ADC_REFERENCE_VOLTAGE 5.07

// Specific sensors:
#define LOAD_VOLTAGE_FACTOR 7.126
#define LOAD_CURRENT_ZERO 2.529
#define LOAD_CURRENT_FACTOR 14.776

#define PANEL_VOLTAGE_FACTOR 7.297
#define PANEL_CURRENT_ZERO 2.529
#define PANEL_CURRENT_FACTOR 14.776

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