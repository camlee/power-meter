#include "display_brightness.h"

#include <Preferences.h>
#include <algorithm>
#include <esp_display_panel.hpp>
#include <time.h>

namespace display_brightness {
namespace {

constexpr char kPreferencesNamespace[] = "appearance";
constexpr char kBrightnessKey[] = "brightness";
constexpr char kDayBrightnessKey[] = "bright_day";
constexpr char kNightBrightnessKey[] = "bright_night";
constexpr char kAutoDayNightKey[] = "bright_auto";
constexpr char kNightPeriodKey[] = "bright_period";

esp_panel::drivers::Backlight* backlightDevice = nullptr;
int currentPercent = kDefaultPercent;
int manualPercent = kDefaultPercent;
int dayPercent = kDefaultPercent;
int nightPercent = kDefaultNightPercent;
bool automaticDayNight = false;
bool appliedNightPeriod = false;

int clampPercent(int percent) {
    return std::clamp(percent, kMinimumPercent, kMaximumPercent);
}

bool clockIsValid() {
    time_t now = time(nullptr);
    struct tm local {};
    localtime_r(&now, &local);
    return local.tm_year >= (2020 - 1900);
}

bool isNightPeriod() {
    time_t now = time(nullptr);
    struct tm local {};
    localtime_r(&now, &local);
    return local.tm_hour >= 22 || local.tm_hour < 6;
}

int selectedPercent() {
    if (!automaticDayNight) return manualPercent;
    return appliedNightPeriod ? nightPercent : dayPercent;
}

bool applySelectedPercent() {
    if (!backlightDevice) return false;
    const int requestedPercent = selectedPercent();
    if (!backlightDevice->setBrightness(requestedPercent)) return false;
    currentPercent = requestedPercent;
    return true;
}

bool saveNightPeriod() {
    Preferences prefs;
    if (!prefs.begin(kPreferencesNamespace, false)) return false;
    const bool saved =
        prefs.putBool(kNightPeriodKey, appliedNightPeriod) == sizeof(bool);
    prefs.end();
    return saved;
}

} // namespace

bool init(esp_panel::drivers::Backlight* backlight) {
    backlightDevice = backlight;

    Preferences prefs;
    if (prefs.begin(kPreferencesNamespace, true)) {
        manualPercent = clampPercent(prefs.getUChar(kBrightnessKey, kDefaultPercent));
        dayPercent = clampPercent(prefs.getUChar(kDayBrightnessKey, manualPercent));
        nightPercent = clampPercent(prefs.getUChar(kNightBrightnessKey, kDefaultNightPercent));
        automaticDayNight = prefs.getBool(kAutoDayNightKey, false);
        appliedNightPeriod = prefs.getBool(kNightPeriodKey, false);
        prefs.end();
    }
    if (clockIsValid()) appliedNightPeriod = isNightPeriod();

    return applySelectedPercent();
}

bool set(int percent) {
    if (!backlightDevice) return false;
    const int requestedPercent = clampPercent(percent);
    if (!backlightDevice->setBrightness(requestedPercent)) return false;
    if (automaticDayNight) {
        if (appliedNightPeriod) nightPercent = requestedPercent;
        else dayPercent = requestedPercent;
    } else {
        manualPercent = requestedPercent;
    }
    currentPercent = requestedPercent;
    return true;
}

int get() {
    return currentPercent;
}

bool save() {
    Preferences prefs;
    if (!prefs.begin(kPreferencesNamespace, false)) return false;
    const char* key = !automaticDayNight ? kBrightnessKey :
                      appliedNightPeriod ? kNightBrightnessKey : kDayBrightnessKey;
    const bool saved = prefs.putUChar(key, static_cast<uint8_t>(currentPercent)) == sizeof(uint8_t);
    prefs.end();
    return saved;
}

bool setAutoDayNight(bool enabled) {
    automaticDayNight = enabled;
    if (automaticDayNight && clockIsValid()) appliedNightPeriod = isNightPeriod();
    const bool applied = applySelectedPercent();

    Preferences prefs;
    if (!prefs.begin(kPreferencesNamespace, false)) return false;
    bool saved = prefs.putBool(kAutoDayNightKey, automaticDayNight) == sizeof(bool);
    if (automaticDayNight) {
        saved &= prefs.putBool(kNightPeriodKey, appliedNightPeriod) == sizeof(bool);
    }
    prefs.end();
    return applied && saved;
}

bool autoDayNight() {
    return automaticDayNight;
}

bool update() {
    if (!automaticDayNight || !clockIsValid()) return false;
    const bool desiredNightPeriod = isNightPeriod();
    if (desiredNightPeriod == appliedNightPeriod) return false;

    appliedNightPeriod = desiredNightPeriod;
    if (!applySelectedPercent()) {
        appliedNightPeriod = !desiredNightPeriod;
        return false;
    }
    saveNightPeriod();
    return true;
}

} // namespace display_brightness
