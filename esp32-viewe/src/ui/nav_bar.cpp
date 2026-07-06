#include "nav_bar.h"

namespace nav_bar {
namespace {

void onClick(lv_event_t* e) {
    ScreenId target = (ScreenId)(uintptr_t)lv_event_get_user_data(e);
    ScreenManager::instance().navigateTo(target);
}

} // namespace

void create(lv_obj_t* parent, ScreenId current) {
    lv_obj_t* bar = lv_obj_create(parent);
    lv_obj_set_size(bar, lv_pct(100), 40);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(bar, 4, 0);

    ScreenManager& sm = ScreenManager::instance();
    for (size_t i = 0; i < sm.screenCount(); i++) {
        ScreenId id = sm.screenIdAt(i);

        lv_obj_t* btn = lv_btn_create(bar);
        lv_obj_set_flex_grow(btn, 1);

        lv_obj_t* label = lv_label_create(btn);
        lv_label_set_text(label, sm.screenTitleAt(i));
        lv_obj_center(label);

        if (id == current) {
            lv_obj_add_state(btn, LV_STATE_DISABLED);
        } else {
            lv_obj_add_event_cb(btn, onClick, LV_EVENT_CLICKED, (void*)(uintptr_t)id);
        }
    }
}

} // namespace nav_bar
