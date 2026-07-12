#pragma once

#include <lvgl.h>

namespace ui_theme {

enum class Mode : uint8_t { Light, Dark, Auto };

void init();
Mode mode();
bool isDark();
void setMode(Mode mode);

lv_color_t background();
lv_color_t surface();
lv_color_t surfaceAlt();
lv_color_t text();
lv_color_t mutedText();
lv_color_t border();
lv_color_t accent();

void styleScreen(lv_obj_t* obj, lv_coord_t padding = 6);
void styleCard(lv_obj_t* obj, lv_coord_t padding = 8);
void stylePrimaryButton(lv_obj_t* obj);
void styleSegment(lv_obj_t* obj);
void styleSectionLabel(lv_obj_t* obj);

} // namespace ui_theme
