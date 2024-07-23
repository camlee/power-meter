#include "../ui.h"
#include "../sensor.h"
#include "../store.h"

#define SENSOR_MAIN_SUBPAGE 0
#define SENSOR_CALIBRATE_SUBPAGE 1
#define SENSOR_CALIBRATING_SUBPAGE 2

#define SENSOR_CURRENT_ZERO_CALIBRATING_PAGE 0
#define SENSOR_CURRENT_FACTOR_CALIBRATING_PAGE 1
#define SENSOR_VOLTAGE_FACTOR_CALIBRATING_PAGE 2


int sensor_subpage = SENSOR_MAIN_SUBPAGE;
bool sensor_enter_calibration = false;
int sensor_calibrating_page = SENSOR_CURRENT_ZERO_CALIBRATING_PAGE;

float calibration_value;

void printCurrent(Display *display, float current){
    display->leftPad(current, 1);
    display->lcd.print(current, 2);
    display->lcd.print(" A");
}

void printVoltage(Display *display, float voltage){
    display->leftPad(voltage, 1);
    display->lcd.print(voltage, 2);
    display->lcd.print(" V");
}

void redrawSensorMainPage(int sensor, Display *display, SensorManager *sensorManager){
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

    display->leftPad(round(power), 2);
    display->lcd.print(power, 1);
    display->lcd.print("W");
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
    }
    if (sensor_calibrating_page == SENSOR_CURRENT_FACTOR_CALIBRATING_PAGE){
        display->lcd.print("A Mult");
    }
    if (sensor_calibrating_page == SENSOR_VOLTAGE_FACTOR_CALIBRATING_PAGE){
        display->lcd.print("V Mult");
    }

    if (sensor == PANEL) {
        display->leftPad(calibration_value, 2);
    }
    if (sensor == LOAD) {
        display->leftPad(calibration_value, 1);
    }
    display->lcd.print(calibration_value, 3);
    display->lcd.print(DISPLAY_NOTHING);
    display->lcd.setCursor(0, 1);

    if (sensor_calibrating_page == SENSOR_CURRENT_ZERO_CALIBRATING_PAGE){
        printCurrent(display, sensorManager->getCurrent(sensor));
        display->lcd.print("  ");
        printCurrent(display, sensorManager->getCurrent(sensor));
    }
    if (sensor_calibrating_page == SENSOR_CURRENT_FACTOR_CALIBRATING_PAGE){
        printCurrent(display, sensorManager->getCurrent(sensor));
        display->lcd.print("  ");
        printCurrent(display, sensorManager->getCurrent(sensor));
    }
    if (sensor_calibrating_page == SENSOR_VOLTAGE_FACTOR_CALIBRATING_PAGE){
        printVoltage(display, sensorManager->getVoltage(sensor));
        display->lcd.print("  ");
        printVoltage(display, sensorManager->getVoltage(sensor));
    }
}


void redrawSensorPage(int sensor, Display *display, SensorManager *sensorManager){
    if (sensor_subpage == SENSOR_MAIN_SUBPAGE) redrawSensorMainPage(sensor, display, sensorManager);
    if (sensor_subpage == SENSOR_CALIBRATE_SUBPAGE) redrawSensorCalibratePage(sensor, display, sensorManager);
    if (sensor_subpage == SENSOR_CALIBRATING_SUBPAGE) redrawSensorCalibrationPage(sensor, display, sensorManager);
}

int buttonsSensorPage(int sensor, int button, Display *display, Store *store){
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
                calibration_value = PANEL_CURRENT_ZERO;
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
            sensor_subpage = SENSOR_MAIN_SUBPAGE;
            display->printLines("Calibration set!", "(not really)");
            return DELAY_REDRAW;
        }
        if (button == UP_BUTTON){
            calibration_value += 0.001;
            return HANDLED;
        }
        if (button == DOWN_BUTTON){
            calibration_value -= 0.001;
            return HANDLED;
        }
        if (button == RIGHT_BUTTON){
            sensor_calibrating_page = incr_with_wrap(sensor_calibrating_page, 0, 2);
            return REDRAW_NOW;
        }
        if (button == LEFT_BUTTON){
            sensor_calibrating_page = decr_with_wrap(sensor_calibrating_page, 0, 2);
            return REDRAW_NOW;
        }
    }

    return BUBBLE;
}

void resetSensorPage(){
    sensor_subpage = SENSOR_MAIN_SUBPAGE;
    sensor_enter_calibration = false;
}