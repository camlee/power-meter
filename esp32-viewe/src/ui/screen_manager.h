#pragma once
#include <lvgl.h>
#include <cstdint>

enum class ScreenId : uint8_t {
    Realtime = 0,
    Info,
    WiFi,
    Count
};

// Lightweight registry + router. Screens are created lazily (on first visit)
// and cached, so navigating back to one just reloads the existing lv_obj_t.
class ScreenManager {
public:
    static ScreenManager& instance();

    // Call once per screen during setup(), before any navigateTo().
    void registerScreen(ScreenId id, const char* title, lv_obj_t* (*createFn)());

    void navigateTo(ScreenId id);

    // Used by nav_bar to build its buttons without hardcoding screen list twice.
    size_t screenCount() const { return registeredCount_; }
    ScreenId screenIdAt(size_t idx) const { return order_[idx]; }
    const char* screenTitleAt(size_t idx) const { return entries_[(size_t)order_[idx]].title; }

private:
    struct Entry {
        const char* title = nullptr;
        lv_obj_t* (*createFn)() = nullptr;
        lv_obj_t* screenObj = nullptr;
    };

    Entry entries_[(size_t)ScreenId::Count];
    ScreenId order_[(size_t)ScreenId::Count];
    size_t registeredCount_ = 0;
};
