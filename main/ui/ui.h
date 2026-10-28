#pragma once

#include "lvgl.h"

/** Build the initial screen. Caller must hold the LVGL lock. */
void ui_init(lv_display_t *disp);
