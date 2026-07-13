#include "linear_progress.h"

#include "../theme/ui_theme.h"

namespace linear_progress {
namespace {

struct State { lv_obj_t* segment; };

void animate(void* value, int32_t position) {
    auto* progress = static_cast<lv_obj_t*>(value);
    auto* state = static_cast<State*>(lv_obj_get_user_data(progress));
    if (!state || !state->segment) return;
    const lv_coord_t width = lv_obj_get_content_width(progress);
    const lv_coord_t segmentWidth = lv_obj_get_width(state->segment);
    lv_obj_set_x(state->segment, (width - segmentWidth) * position / 1000);
}

void start(lv_obj_t* progress) {
    lv_anim_del(progress, animate);
    lv_anim_t animation;
    lv_anim_init(&animation);
    lv_anim_set_var(&animation, progress);
    lv_anim_set_exec_cb(&animation, animate);
    lv_anim_set_values(&animation, 0, 1000);
    lv_anim_set_time(&animation, 850);
    lv_anim_set_repeat_count(&animation, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&animation, lv_anim_path_linear);
    lv_anim_start(&animation);
}

} // namespace

lv_obj_t* create(lv_obj_t* parent) {
    lv_obj_t* progress = lv_obj_create(parent);
    lv_obj_remove_style_all(progress);
    lv_obj_set_size(progress, lv_pct(100), 5);
    lv_obj_align(progress, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_add_flag(progress, LV_OBJ_FLAG_FLOATING | LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_clear_flag(progress, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(progress, ui_theme::surfaceAlt(), 0);
    lv_obj_set_style_bg_opa(progress, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(progress, 0, 0);

    lv_obj_t* segment = lv_obj_create(progress);
    lv_obj_remove_style_all(segment);
    lv_obj_set_size(segment, lv_pct(32), lv_pct(100));
    lv_obj_set_style_bg_color(segment, ui_theme::accent(), 0);
    lv_obj_set_style_bg_opa(segment, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(segment, 0, 0);
    auto* state = new State{segment};
    lv_obj_set_user_data(progress, state);
    lv_obj_add_flag(progress, LV_OBJ_FLAG_HIDDEN);
    return progress;
}

void show(lv_obj_t* progress) {
    if (!progress) return;
    lv_obj_clear_flag(progress, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(progress);
    start(progress);
}

void hide(lv_obj_t* progress) {
    if (!progress) return;
    lv_anim_del(progress, animate);
    lv_obj_add_flag(progress, LV_OBJ_FLAG_HIDDEN);
}

} // namespace linear_progress
