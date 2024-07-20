#include "ui.h"
#include "buttons.h"

const char* weekday_to_text(byte weekday){
    switch (weekday){
        case 0: return "Unknown";
        case 1: return "Sun";
        case 2: return "Mon";
        case 3: return "Tue";
        case 4: return "Wed";
        case 5: return "Thu";
        case 6: return "Fri";
        case 7: return "Sat";
    }
    return "Other";
}

byte incr_with_wrap(byte value, byte min, byte max){
    if (value == 255){
        return min;
    }
    value += 1;
    if (value > max){
        value = min;
    }
    return value;
}

byte decr_with_wrap(byte value, byte min, byte max){
    if (value == 0){
        return max;
    }
    value -= 1;
    if (value < min){
        value = max;
    }
    return value;
}

UI::UI(Display* display, SensorManager* sensorManager, Store* store) :
    page(DEFAULT_PAGE),
    subpage(0),
    editing(false),
    nextRefresh(0),
    blinkVisibility(true),
    weekday(0),
    hour(12),
    minute(0),
    timeSetAt(),

    display(display),
    sensorManager(sensorManager),
    store(store)
{}

void UI::setup(){
}

void UI::refresh(){
    if (millis() > nextRefresh ){
        nextRefresh = millis() + refreshPeriodMillis;

        redraw();
    }
}

void UI::redraw(){
    if (page == SUMMARY_PAGE) {
        redrawSummaryPage();
    }

    if (page == AC_PAGE) {
        redrawACPage();
    }

    if (page == PANEL_PAGE) {
        redrawPanelPage();
    }

    if (page == DETAILS_PAGE) {
        redrawDetailsPage();
    }

    if (page == ENERGY_PAGE) {
        redrawEnergyPage();
    }

    if (page == DEBUG_PAGE) {
        redrawDebugPage();
    }

    if (page == TIME_PAGE) {
        redrawTimePage();
    }

}


void UI::redrawSummaryPage(){
    float power = sensorManager->getPower(PANEL);
    float duty = sensorManager->getDuty(PANEL);
    float theoretical_power = round(power / duty);
    power = round(power);

    display->lcd.setCursor(0, 0);
    display->lcd.print("Sun");
    display->leftPad(theoretical_power, 3);
    display->lcd.print(theoretical_power, 0);
    display->lcd.print("W");

    display->lcd.print(" In");
    display->leftPad(power, 3);
    display->lcd.print(power, 0);
    display->lcd.print("W");

    display->lcd.print(DISPLAY_NOTHING);

    power = round(sensorManager->getPower(LOAD) + sensorManager->getPower(LOAD2));
    float bat_percent = round(sensorManager->getBatLevel() / sensorManager->getBatCapacity() * 100.0);
    display->lcd.setCursor(0, 1);
    display->lcd.print("Out");
    display->leftPad(power, 3);
    display->lcd.print(power, 0);
    display->lcd.print("W");

    display->lcd.print(" Bat");
    display->leftPad(bat_percent, 2);
    display->lcd.print(bat_percent, 0);
    display->lcd.print("%");
    display->lcd.print(DISPLAY_NOTHING);
}

void UI::redrawACPage(){
    float voltage = sensorManager->getVoltage(LOAD);
    float current = sensorManager->getCurrent(LOAD);
    float power = sensorManager->getPower(LOAD);
    float energy = sensorManager->getEnergy(LOAD);

    display->lcd.setCursor(0, 0);
    display->lcd.print("Out");

    partialDrawSensor(power, voltage, current, energy, 0, false);
}


void UI::redrawPanelPage(){
    float voltage = sensorManager->getVoltage(PANEL);
    float current = sensorManager->getCurrent(PANEL);
    float power = sensorManager->getPower(PANEL);
    float energy = sensorManager->getEnergy(PANEL);
    float duty = sensorManager->getDuty(PANEL) * 100.0;

    display->lcd.setCursor(0, 0);
    display->lcd.print("In");

    bool show_duty = duty < 90 && power > 30;
    partialDrawSensor(power, voltage, current, energy, duty, show_duty);
}

void UI::partialDrawSensor(float power, float voltage, float current, float energy, float duty, bool show_duty){
    display->leftPad(round(power), 2);
    display->lcd.print(power, 1);
    display->lcd.print("W");
    if (!show_duty){
        display->leftPad(energy / 3600.0, 4);
        display->lcd.print(energy / 3600.0, 0);
        display->lcd.print("Wh");
    } else {
        display->lcd.print(" PWM ");
        display->leftPad(duty, 1);
        display->lcd.print(duty, 0);
        display->lcd.print("%");
    }
    display->lcd.print(DISPLAY_NOTHING);

    display->lcd.setCursor(0, 1);
    display->leftPad(current, 1);
    display->lcd.print(current, 2);
    display->lcd.print(" A");
    display->leftPad(voltage, 3);
    display->lcd.print(voltage, 2);
    display->lcd.print(" V");
    display->lcd.print(DISPLAY_NOTHING);
}

void UI::redrawEnergyPage(){
    float energy = sensorManager->getEnergy(PANEL) / 3600.0;

    display->lcd.setCursor(0, 0);
    display->lcd.print("In ");
    display->leftPad(energy, 8);
    display->lcd.print(energy, 0);
    display->lcd.print(" Wh");
    display->lcd.print(DISPLAY_NOTHING);

    display->lcd.setCursor(0, 1);
    energy = (sensorManager->getEnergy(LOAD) + sensorManager->getEnergy(LOAD2)) / 3600.0;
    display->lcd.print("Out");
    display->leftPad(energy, 8);
    display->lcd.print(energy, 0);
    display->lcd.print(" Wh");
    display->lcd.print(DISPLAY_NOTHING);
}

void UI::redrawDebugPage(){
    unsigned long uptime = millis() / 60000; // In minutes
    display->lcd.setCursor(0, 0);
    display->lcd.print("Up");
    display->leftPad(uptime, 3);
    display->lcd.print(uptime);
    display->lcd.print("m");

    display->lcd.print(" Last");
    uptime = store->getLastUptime(); // In minutes
    display->leftPad(uptime, 2);
    display->lcd.print(uptime);
    display->lcd.print("m");
    display->lcd.print(DISPLAY_NOTHING);

    display->lcd.setCursor(0, 1);
    display->lcd.print("Boot");
    display->leftPad(store->getRestarts(), 2);
    display->lcd.print(store->getRestarts());

    display->lcd.print(" Write");
    display->leftPad(store->writes, 2);
    display->lcd.print(store->writes);
    display->lcd.print(DISPLAY_NOTHING);
}

    //     if (page == HISTORY_PAGE){
    //         display->lcd.setCursor(0, 0);
    //         display->lcd.print("History Page");
    //         display->lcd.print(DISPLAY_NOTHING);
    //         display->lcd.setCursor(0, 1);
    //         display->lcd.print("TODO");
    //         display->lcd.print(DISPLAY_NOTHING);
    //         // energy = store->getHistoricalEnergy(PANEL) / 3600.0;

    //         // display->lcd.setCursor(0, 0);
    //         // display->lcd.print("Hist In ");
    //         // display->leftPad(energy, 4);
    //         // display->lcd.print(energy, 0);
    //         // display->lcd.print(" Wh");
    //         // display->lcd.print(DISPLAY_NOTHING);

    //         // display->lcd.setCursor(0, 1);
    //         // energy = (store->getHistoricalEnergy(LOAD) + store->getHistoricalEnergy(LOAD2)) / 3600.0;
    //         // display->lcd.print("Hist Out");
    //         // display->leftPad(energy, 4);
    //         // display->lcd.print(energy, 0);
    //         // display->lcd.print(" Wh");
    //         // display->lcd.print(DISPLAY_NOTHING);
    //     }

void UI::redrawDetailsPage(){
    display->lcd.setCursor(0, 0);
    display->lcd.print("Bat ");
    display->lcd.print(sensorManager->getBatLevel() / 3600.0, 0);
    display->lcd.print("/");
    display->lcd.print(sensorManager->getBatCapacity() / 3600.0, 0);
    display->lcd.print(" Wh");
    display->lcd.print(DISPLAY_NOTHING);

    display->lcd.setCursor(0, 1);
    display->lcd.print(sensorManager->getBatMin() / 3600.0, 0);
    display->lcd.print("<->");
    display->lcd.print(sensorManager->getBatMax() / 3600.0, 0);
    display->lcd.print("Wh");
    // float bat_percent = round((sensorManager->getBatMax() - sensorManager->getBatMin()) / sensorManager->getBatCapacity() * 1000)/10;
    // display->leftPad(bat_percent, 2);
    // display->lcd.print(bat_percent, 1);
    // display->lcd.print("%");

    display->lcd.print(DISPLAY_NOTHING);
}

void UI::redrawTimePage(){
    unsigned long now;
    display->lcd.setCursor(0, 0);
    if (!editing) {
        if (weekday == 0) {
            display->printLine1("Time Unknown");
            display->printLine2("Set using SELECT");
        } else {
            display->printLine1("Time");
            display->lcd.setCursor(0, 1);
            now = millis();
            printTime(getWeekday(now), getHour(now), getMinute(now));
            display->lcd.print(DISPLAY_NOTHING);
        }
    } else {
        if (subpage == 0){
            display->printLine1("Set Day of week:");
            display->lcd.setCursor(0, 1);
            printTime(weekday, hour, minute, true, false, false);
            display->lcd.print(DISPLAY_NOTHING);
        }
        if (subpage == 1){
            display->printLine1("Set Hour:");
            display->lcd.setCursor(0, 1);
            printTime(weekday, hour, minute, false, true, false);
            display->lcd.print(DISPLAY_NOTHING);
        }
        if (subpage == 2){
            display->printLine1("Set Minute:");
            display->lcd.setCursor(0, 1);
            printTime(weekday, hour, minute, false, false, true);
            display->lcd.print(DISPLAY_NOTHING);
        }
    }

}

void UI::printTime(byte day, byte hour, byte minute, bool blink_day, bool blink_hour, bool blink_minute){
    bool blink = getBlink();

    if (!blink_day || blink){
        display->lcd.print(weekday_to_text(weekday));
    } else{
        display->lcd.print("   "); // Three characters for abbreviated weekday.
    }
    display->lcd.print(" ");

    boolean pm = hour >= 12;
    hour = hour % 12;
    if (hour == 0){
        hour = 12;
    }

    if (!blink_hour || blink){
        if (hour < 10) {
            display->lcd.print("0");
        }
        display->lcd.print(hour);
    } else {
        display->lcd.print("  ");
    }

    display->lcd.print(":");

    if (!blink_minute || blink){
        if (minute < 10) {
            display->lcd.print("0");
        }
        display->lcd.print(minute);
    } else {
        display->lcd.print("  ");
    }
    display->lcd.print(" ");

    if (!blink_hour || blink){
        if (pm){
            display->lcd.print("PM");
        } else {
            display->lcd.print("AM");
        }
    } else {
        display->lcd.print("  ");
    }
}


void UI::handleButton(int button){
    // Order of code matters here, we return early when applying a button press.
    // More general near the bottom. More specific near the top.
    // This is to avoid a button press doing multiple things and having to
    // code all specific cases.

    unsigned long now;

    // Time page:
    if (page == TIME_PAGE){
        if (button == SELECT_BUTTON){
            if (editing){
                timeSetAt = millis();
                editing = false;

            } else {
                editing = true;

                if (weekday == 0){ // Time wasn't known previously
                    weekday = 1;
                } else {
                    now = millis();
                    weekday = getWeekday(now);
                    hour = getHour(now);
                    minute = getMinute(now);
                }
            }
            redrawASAP();
            return;
        }
        if (editing){
            if (button == UP_BUTTON){
                if (subpage == 0) weekday = incr_with_wrap(weekday, 1, 7);
                if (subpage == 1) hour = incr_with_wrap(hour, 0, 23);
                if (subpage == 2) minute = incr_with_wrap(minute, 0, 59);

                redrawASAP();
                return;
            }
            if (button == DOWN_BUTTON){
                if (subpage == 0) weekday = decr_with_wrap(weekday, 1, 7);
                if (subpage == 1) hour = decr_with_wrap(hour, 0, 23);
                if (subpage == 2) minute = decr_with_wrap(minute, 0, 59);

                redrawASAP();
                return;
            }
            if (button == RIGHT_BUTTON){
                subpage = incr_with_wrap(subpage, 0, 2);
                redrawASAP();
                return;
            }
            if (button == LEFT_BUTTON){
                subpage = decr_with_wrap(subpage, 0, 2);
                redrawASAP();
                return;
            }
        }
    }


    // Debug page:
    if (page == DEBUG_PAGE && button == SELECT_BUTTON) {
        store->persistNow();
        display->lcd.setCursor(0, 0);
        display->lcd.print("Saved!");
        display->lcd.print(DISPLAY_NOTHING);

        display->lcd.setCursor(0, 1);
        display->lcd.print(DISPLAY_NOTHING);

        delayRedraw();
        return;
    }

    // Energy page:
    if (page == ENERGY_PAGE && button == SELECT_BUTTON) {
        sensorManager->setup(0.0, 0.0);
        store->refresh(0.0, 0.0);
        store->persistNow();

        display->lcd.setCursor(0, 0);
        display->lcd.print("Energy reset!");
        display->lcd.print(DISPLAY_NOTHING);

        display->lcd.setCursor(0, 1);
        display->lcd.print(DISPLAY_NOTHING);

        delayRedraw();
        return;
    }

    // Cycle between pages:
    if (button == LEFT_BUTTON){
        editing = false;
        page = decr_with_wrap(page, MIN_PAGE, MAX_PAGE);
        redrawASAP();
        return;
    }
    if (button == RIGHT_BUTTON){
        editing = false;
        page = incr_with_wrap(page, MIN_PAGE, MAX_PAGE);
        redrawASAP();
        return;
    }

    // Brightness controls:
    if (button == UP_BUTTON){
        display->brightnessUp();
    }
    if (button == DOWN_BUTTON){
        display->brightnessDown();
    }
    if (button == UP_BUTTON || button == DOWN_BUTTON){
        display->lcd.setCursor(0, 0);
        display->lcd.print("Brightness: ");
        display->lcd.print(display->getBrightness() * 100 / display->brightnessIncrements);
        display->lcd.print("%     ");

        display->lcd.setCursor(0, 1);
        display->lcd.print(DISPLAY_NOTHING);

        delayRedraw();
        return;
    }

}

void UI::redrawASAP(){
    nextRefresh = millis();
}

void UI::delayRedraw(){
    nextRefresh = millis() + refreshPeriodMillis * 2;
}

boolean UI::getBlink(){
    blinkVisibility = !blinkVisibility;
    return blinkVisibility;
}



byte UI::getWeekday(unsigned long time){
    unsigned long minutes_elapsed = (time - timeSetAt) / 60000;
    unsigned long new_minute = minute + minutes_elapsed;
    unsigned long new_hour = hour + new_minute / 60;
    byte new_day = weekday + new_hour / 24;

    return ((new_day - 1) % 7) + 1;
}

byte UI::getHour(unsigned long time){
    unsigned long minutes_elapsed = (time - timeSetAt) / 60000;
    unsigned long new_minute = minute + minutes_elapsed;
    unsigned long new_hour = hour + new_minute / 60;
    return new_hour % 24;
}

byte UI::getMinute(unsigned long time){
    unsigned long minutes_elapsed = (time - timeSetAt) / 60000;
    unsigned long new_minute = minute + minutes_elapsed;
    return new_minute % 60;
}