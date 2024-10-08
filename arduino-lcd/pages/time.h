#include "../display.h"
#include "../ui.h"
#include "../util.h"

#define UNKNOWN_WEEKDAY 8
byte weekday; // 0 for Sunday, 1 Monday, ... 6 for Saturday. 8 for Unknown
byte hour;
byte minute;
unsigned long time_set_at;
bool time_page_editing = false;
unsigned int time_subpage = 0;

const char* weekday_to_text(byte weekday){
    switch (weekday){
        case UNKNOWN_WEEKDAY: return "Unknown";
        case 0: return "Sun";
        case 1: return "Mon";
        case 2: return "Tue";
        case 3: return "Wed";
        case 4: return "Thu";
        case 5: return "Fri";
        case 6: return "Sat";
    }
    return "Other";
}

byte get_weekday(unsigned long time){
    unsigned long minutes_elapsed = (time - time_set_at) / 60000;
    unsigned long new_minute = minute + minutes_elapsed;
    unsigned long new_hour = hour + new_minute / 60;
    byte new_day = weekday + new_hour / 24;

    return new_day % 7;
}

byte get_hour(unsigned long time){
    unsigned long minutes_elapsed = (time - time_set_at) / 60000;
    unsigned long new_minute = minute + minutes_elapsed;
    unsigned long new_hour = hour + new_minute / 60;
    return new_hour % 24;
}

byte get_minute(unsigned long time){
    unsigned long minutes_elapsed = (time - time_set_at) / 60000;
    unsigned long new_minute = minute + minutes_elapsed;
    return new_minute % 60;
}


void print_time(Display *display, byte weekday, byte hour, byte minute, bool blink_day=false, bool blink_hour=false, bool blink_minute=false){
    bool blink = display->getBlink();

    if (!blink_day || blink){
        display->lcd.print(weekday_to_text(weekday));
    } else{
        display->lcd.print("   "); // Three characters for abbreviated weekday.
    }
    display->lcd.print(" ");

    bool pm = hour >= 12;
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

void redrawTimePage(Display* display){
    unsigned long now;
    display->lcd.setCursor(0, 0);
    if (!time_page_editing) {
        if (weekday == UNKNOWN_WEEKDAY) {
            display->printLine1("Time Unknown");
            display->printLine2("Set using SELECT");
        } else {
            display->lcd.setCursor(0, 0);
            display->lcd.print("Time ");
            now = millis();
            // display->lcd.print(((double)now)/1000, 1);
            display->lcd.print(DISPLAY_NOTHING);
            display->lcd.setCursor(0, 1);
            print_time(display, get_weekday(now), get_hour(now), get_minute(now));
            display->lcd.print(DISPLAY_NOTHING);
        }
    } else {
        if (time_subpage == 0){
            display->printLine1("Set Day of week:");
            display->lcd.setCursor(0, 1);
            print_time(display, weekday, hour, minute, true, false, false);
            display->lcd.print(DISPLAY_NOTHING);
        }
        if (time_subpage == 1){
            display->printLine1("Set Hour:");
            display->lcd.setCursor(0, 1);
            print_time(display, weekday, hour, minute, false, true, false);
            display->lcd.print(DISPLAY_NOTHING);
        }
        if (time_subpage == 2){
            display->printLine1("Set Minute:");
            display->lcd.setCursor(0, 1);
            print_time(display, weekday, hour, minute, false, false, true);
            display->lcd.print(DISPLAY_NOTHING);
        }
    }

}

int buttonsTimePage(int button){
    unsigned long now;
    if (button == SELECT_BUTTON){
        if (time_page_editing){
            time_set_at = millis();
            time_page_editing = false;

        } else {
            time_page_editing = true;

            if (weekday == UNKNOWN_WEEKDAY){
                weekday = 0;
            } else {
                now = millis();
                weekday = get_weekday(now);
                hour = get_hour(now);
                minute = get_minute(now);
            }
        }
        return REDRAW_NOW;
    }
    if (time_page_editing){
        if (button == UP_BUTTON){
            if (time_subpage == 0) weekday = incr_with_wrap(weekday, 0, 6);
            if (time_subpage == 1) hour = incr_with_wrap(hour, 0, 23);
            if (time_subpage == 2) minute = incr_with_wrap(minute, 0, 59);

            return REDRAW_NOW;
        }
        if (button == DOWN_BUTTON){
            if (time_subpage == 0) weekday = decr_with_wrap(weekday, 0, 6);
            if (time_subpage == 1) hour = decr_with_wrap(hour, 0, 23);
            if (time_subpage == 2) minute = decr_with_wrap(minute, 0, 59);

            return REDRAW_NOW;
        }
        if (button == RIGHT_BUTTON){
            time_subpage = incr_with_wrap(time_subpage, 0, 2);
            return REDRAW_NOW;
        }
        if (button == LEFT_BUTTON){
            time_subpage = decr_with_wrap(time_subpage, 0, 2);
            return REDRAW_NOW;
        }
    }

    return BUBBLE;
}