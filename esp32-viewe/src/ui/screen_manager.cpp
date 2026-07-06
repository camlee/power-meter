#include "screen_manager.h"

ScreenManager& ScreenManager::instance() {
    static ScreenManager inst;
    return inst;
}

void ScreenManager::registerScreen(ScreenId id, const char* title, lv_obj_t* (*createFn)()) {
    entries_[(size_t)id].title = title;
    entries_[(size_t)id].createFn = createFn;
    order_[registeredCount_++] = id;
}

void ScreenManager::navigateTo(ScreenId id) {
    Entry& entry = entries_[(size_t)id];
    if (entry.screenObj == nullptr) {
        entry.screenObj = entry.createFn();
    }
    lv_scr_load_anim(entry.screenObj, LV_SCR_LOAD_ANIM_MOVE_LEFT, 200, 0, false);
}
