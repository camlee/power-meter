#pragma once
#include <lvgl.h>
#include <vector>

// Signature: screens receive their parent tab page.
typedef lv_obj_t* (*ScreenCreateFunc)(lv_obj_t* parent);

class ScreenManager {
public:
    static ScreenManager& instance() {
        static ScreenManager instance;
        return instance;
    }

    // Initialize the root layout (Tabview)
    void init();

    // Queue up a screen to be created
    void registerScreen(const char* name, ScreenCreateFunc createFunc);

    // Build all the registered tabs
    void build();

    // Selects a registered top-level screen. This is used by full-screen
    // workflows that hand off to an existing editor rather than duplicating
    // that editor inside an overlay.
    void showScreen(size_t index);

private:
    ScreenManager() = default;

    lv_obj_t* root_scr = nullptr;
    lv_obj_t* tabview = nullptr;

    struct ScreenDef {
        const char* name;
        ScreenCreateFunc createFunc;
        lv_obj_t* tab = nullptr;
        lv_obj_t* content = nullptr;
    };

    std::vector<ScreenDef> screens;

    static void tabChangedCb(lv_event_t* event);
    void notifyActiveScreen();
};
