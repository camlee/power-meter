#pragma once
#include <esp_display_panel.hpp>

// Initializes the display panel board and the LVGL port for it.
// Handles board/bus-specific quirks internally (e.g. tearing-avoidance
// buffer config), which activate or no-op automatically depending on
// which board is selected in esp_panel_board_supported_conf.h.
//
// Returns the initialized Board*, owned for the lifetime of the program.
esp_panel::board::Board* initDisplayAndLvgl();
