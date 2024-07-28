#include "../ui.h"
#include "../sensor.h"

#define SENSOR_MAIN_SUBPAGE 0
#define SENSOR_CALIBRATE_SUBPAGE 1
#define SENSOR_CALIBRATING_SUBPAGE 2

#define SENSOR_CALIBRATING_PAGE_MIN 0
#define SENSOR_CALIBRATING_PAGE_DEFAULT 0
#define SENSOR_CURRENT_ZERO_CALIBRATING_PAGE 0
#define SENSOR_CURRENT_FACTOR_CALIBRATING_PAGE 1
#define SENSOR_VOLTAGE_FACTOR_CALIBRATING_PAGE 2
#define SENSOR_SAVE_CALIBRATING_PAGE 3
#define SENSOR_RESET_CALIBRATING_PAGE 4
#define SENSOR_CANCEL_CALIBRATING_PAGE 5
#define SENSOR_CALIBRATING_PAGE_MAX 5

#define SENSOR_REFRESH_PERIOD_MILLIS 1000

unsigned long int sensor_next_refresh = 0;
int sensor_previous_sensor = -1;

int sensor_subpage = SENSOR_MAIN_SUBPAGE;
bool sensor_enter_calibration = false;
int sensor_calibrating_page = SENSOR_CALIBRATING_PAGE_DEFAULT;

float sensor_current_zero_calibration_value;
float sensor_current_factor_calibration_value;
float sensor_voltage_factor_calibration_value;

void printCurrent(Display *display, float current){
    if (abs(current) < 0.005) current = 0.0; // To avoid bouncing around -0.0
    display->leftPad(current, 1);
    display->lcd.print(current, 2);
    display->lcd.print(" A");
}

void printVoltage(Display *display, float voltage){
    display->leftPad(voltage, 1);
    display->lcd.print(voltage, 2);
    display->lcd.print(" V");
}

void printPower(Display *display, float power){
    if (abs(power) < 0.005) power = 0.0; // To avoid bouncing around -0.0
    display->leftPad(round(power), 2);
    display->lcd.print(power, 1);
    display->lcd.print("W");
}

void redrawSensorMainPage(int sensor, Display *display, SensorManager *sensorManager){
    if (sensor == sensor_previous_sensor && millis() <= sensor_next_refresh ){
        return; // Don't refresh too often cause it makes the numbers
                // unreadable as they change slightly
    } else {
        sensor_previous_sensor = sensor;
        sensor_next_refresh = millis() + SENSOR_REFRESH_PERIOD_MILLIS;
    }

    float voltage = sensorManager->getVoltage(sensor);
    float current = sensorManager->getCurrent(sensor);
    float power = sensorManager->getPower(sensor);
    float energy = sensorManager->getEnergy(sensor);
    float duty;

    display->lcd.setCursor(0, 0);
    if (sensor == PANEL){
        duty = sensorManager->getDuty(PANEL) * 100.0;
        display->lcd.print("In");
    }
    if (sensor == LOAD){
        display->lcd.print("Out");
    }

    printPower(display, power);
    if (sensor == PANEL && duty < 90 && power > 30){
        display->lcd.print(" PWM ");
        display->leftPad(duty, 1);
        display->lcd.print(duty, 0);
        display->lcd.print("%");
    } else {
        display->leftPad(energy / 3600.0, 4);
        display->lcd.print(energy / 3600.0, 0);
        display->lcd.print("Wh");
    }
    display->lcd.print(DISPLAY_NOTHING);

    display->lcd.setCursor(0, 1);
    printCurrent(display, current);
    display->lcd.print("  ");
    printVoltage(display, voltage);
    display->lcd.print(DISPLAY_NOTHING);
}

void redrawSensorCalibratePage(int sensor, Display * display, SensorManager *sensorManager){
    display->lcd.setCursor(0, 0);
    display->lcd.print("Calibrate ");
    if (sensor == PANEL) {
        display->lcd.print("In?");
    }
    if (sensor == LOAD) {
        display->lcd.print("Out?");
    }
    display->lcd.print(DISPLAY_NOTHING);
    display->lcd.setCursor(0, 1);
    display->printYesNoSelection(sensor_enter_calibration);
}

void redrawSensorCalibrationPage(int sensor, Display *display, SensorManager *sensorManager){
    display->lcd.setCursor(0, 0);
    if (sensor == PANEL) {
        display->lcd.print("In ");
    }
    if (sensor == LOAD) {
        display->lcd.print("Out ");
    }

    if (sensor_calibrating_page == SENSOR_CURRENT_ZERO_CALIBRATING_PAGE){
        display->lcd.print("A Zero");
        display->leftPad(sensor_current_zero_calibration_value, 1);
        display->lcd.print(sensor_current_zero_calibration_value, 3);
        display->lcd.print(DISPLAY_NOTHING);
        display->lcd.setCursor(0, 1);
        printCurrent(display, sensorManager->getCurrent(sensor));
        display->lcd.print("  ");
        printCurrent(display, sensorManager->getCurrent(sensor,
            sensor_current_zero_calibration_value, sensor_current_factor_calibration_value));
    }
    if (sensor_calibrating_page == SENSOR_CURRENT_FACTOR_CALIBRATING_PAGE){
        display->lcd.print("A Mult");
        display->leftPad(sensor_current_factor_calibration_value, 1);
        display->lcd.print(sensor_current_factor_calibration_value, 3);
        display->lcd.print(DISPLAY_NOTHING);
        display->lcd.setCursor(0, 1);
        printCurrent(display, sensorManager->getCurrent(sensor));
        display->lcd.print("  ");
        printCurrent(display, sensorManager->getCurrent(sensor,
            sensor_current_zero_calibration_value, sensor_current_factor_calibration_value));
    }
    if (sensor_calibrating_page == SENSOR_VOLTAGE_FACTOR_CALIBRATING_PAGE){
        display->lcd.print("V Mult");
        display->leftPad(sensor_voltage_factor_calibration_value, 1);
        display->lcd.print(sensor_voltage_factor_calibration_value, 3);
        display->lcd.print(DISPLAY_NOTHING);
        display->lcd.setCursor(0, 1);
        printVoltage(display, sensorManager->getVoltage(sensor));
        display->lcd.print("  ");
        printVoltage(display, sensorManager->getVoltage(sensor));
    }
    if (sensor_calibrating_page == SENSOR_SAVE_CALIBRATING_PAGE){
        display->printLines("Save Changes?", "");
    }
    if (sensor_calibrating_page == SENSOR_RESET_CALIBRATING_PAGE){
        display->printLines("Reset to ", "defaults?");
    }
    if (sensor_calibrating_page == SENSOR_CANCEL_CALIBRATING_PAGE){
        display->printLines("Cancel?", "");
    }
}


void redrawSensorPage(int sensor, Display *display, SensorManager *sensorManager){
    if (sensor_subpage == SENSOR_MAIN_SUBPAGE) redrawSensorMainPage(sensor, display, sensorManager);
    if (sensor_subpage == SENSOR_CALIBRATE_SUBPAGE) redrawSensorCalibratePage(sensor, display, sensorManager);
    if (sensor_subpage == SENSOR_CALIBRATING_SUBPAGE) redrawSensorCalibrationPage(sensor, display, sensorManager);
}

int buttonsSensorPage(int sensor, int button, Display *display, SensorManager *sensorManager){
    if (sensor_subpage == SENSOR_MAIN_SUBPAGE){
        if (button == SELECT_BUTTON) {
            sensor_subpage = SENSOR_CALIBRATE_SUBPAGE;
            return REDRAW_NOW;
        }
    }
    if (sensor_subpage == SENSOR_CALIBRATE_SUBPAGE) {
        if (button == SELECT_BUTTON) {
            if (sensor_enter_calibration){
                sensor_subpage = SENSOR_CALIBRATING_SUBPAGE;
                sensor_enter_calibration = false;
                sensor_current_zero_calibration_value = sensorManager->getCurrentZero(sensor);
                sensor_current_factor_calibration_value = sensorManager->getCurrentFactor(sensor);
                sensor_voltage_factor_calibration_value = sensorManager->getVoltageFactor(sensor);
                return REDRAW_NOW;
            } else {
                sensor_subpage = SENSOR_MAIN_SUBPAGE;
            }
            sensor_subpage = SENSOR_MAIN_SUBPAGE;
            return REDRAW_NOW;
        }
        if (button == LEFT_BUTTON || button == RIGHT_BUTTON) {
            sensor_enter_calibration = !sensor_enter_calibration;
            return REDRAW_NOW;
        }
    }
    if (sensor_subpage == SENSOR_CALIBRATING_SUBPAGE) {
        if (button == SELECT_BUTTON){
            if (sensor_calibrating_page == SENSOR_SAVE_CALIBRATING_PAGE) {
                sensorManager->setCurrentZero(sensor, sensor_current_zero_calibration_value);
                sensorManager->setCurrentFactor(sensor, sensor_current_factor_calibration_value);
                sensorManager->setVoltageFactor(sensor, sensor_voltage_factor_calibration_value);
                display->printLines("Calibration", "set!");
                sensor_subpage = SENSOR_MAIN_SUBPAGE;
                sensor_calibrating_page = SENSOR_CALIBRATING_PAGE_DEFAULT;
                return DELAY_REDRAW;
            }
            if (sensor_calibrating_page == SENSOR_RESET_CALIBRATING_PAGE) {
                sensorManager->setCurrentZero(sensor);
                sensorManager->setCurrentFactor(sensor);
                sensorManager->setVoltageFactor(sensor);
                display->printLines("Calibration", "reset!");
                sensor_subpage = SENSOR_MAIN_SUBPAGE;
                sensor_calibrating_page = SENSOR_CALIBRATING_PAGE_DEFAULT;
                return DELAY_REDRAW;
            }
            if (sensor_calibrating_page == SENSOR_CANCEL_CALIBRATING_PAGE){
                sensor_subpage = SENSOR_MAIN_SUBPAGE;
                sensor_calibrating_page = SENSOR_CALIBRATING_PAGE_DEFAULT;
                return REDRAW_NOW;
            }
        }
        if (button == UP_BUTTON){
            if (sensor_calibrating_page == SENSOR_CURRENT_ZERO_CALIBRATING_PAGE){
                sensor_current_zero_calibration_value += 0.001;
            }
            if (sensor_calibrating_page == SENSOR_CURRENT_FACTOR_CALIBRATING_PAGE){
                sensor_current_factor_calibration_value += 0.001;
            }
            if (sensor_calibrating_page == SENSOR_VOLTAGE_FACTOR_CALIBRATING_PAGE){
                sensor_voltage_factor_calibration_value += 0.001;
            }
            return HANDLED;
        }
        if (button == DOWN_BUTTON){
            if (sensor_calibrating_page == SENSOR_CURRENT_ZERO_CALIBRATING_PAGE){
                sensor_current_zero_calibration_value -= 0.001;
            }
            if (sensor_calibrating_page == SENSOR_CURRENT_FACTOR_CALIBRATING_PAGE){
                sensor_current_factor_calibration_value -= 0.001;
            }
            if (sensor_calibrating_page == SENSOR_VOLTAGE_FACTOR_CALIBRATING_PAGE){
                sensor_voltage_factor_calibration_value -= 0.001;
            }
            return HANDLED;
        }
        if (button == RIGHT_BUTTON){
            sensor_calibrating_page = incr_with_wrap(sensor_calibrating_page, SENSOR_CALIBRATING_PAGE_MIN, SENSOR_CALIBRATING_PAGE_MAX);
            return REDRAW_NOW;
        }
        if (button == LEFT_BUTTON){
            sensor_calibrating_page = decr_with_wrap(sensor_calibrating_page, SENSOR_CALIBRATING_PAGE_MIN, SENSOR_CALIBRATING_PAGE_MAX);
            return REDRAW_NOW;
        }
    }

    return BUBBLE;
}

void resetSensorPage(){
    sensor_subpage = SENSOR_MAIN_SUBPAGE;
    sensor_enter_calibration = false;
    sensor_next_refresh = 0;
    sensor_previous_sensor = -1;
}