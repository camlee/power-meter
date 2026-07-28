#pragma once

namespace esp_panel::drivers {
class Backlight;
}

namespace display_brightness {

constexpr int kMinimumPercent = 1;
constexpr int kMaximumPercent = 100;
constexpr int kDefaultPercent = 100;
constexpr int kDefaultNightPercent = 10;

// Loads the saved level and applies it to the board-owned PWM backlight.
bool init(esp_panel::drivers::Backlight* backlight);

// Applies a level immediately without writing flash.
bool set(int percent);
int get();

// Persists the currently applied level.
bool save();

// Auto mode maintains independent day and night levels. The slider edits the
// level for the currently active period.
bool setAutoDayNight(bool enabled);
bool autoDayNight();

// Applies a day/night transition when trustworthy local time becomes
// available or crosses 06:00/22:00. Returns true when the level changed.
bool update();

} // namespace display_brightness
