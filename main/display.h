#pragma once

#include "esp_err.h"
#include "lvgl.h"

#define SIPWIRE_LCD_H_RES 320
#define SIPWIRE_LCD_V_RES 240

/**
 * Initialize the LVGL port (task + tick) and register the display.
 *
 * Until a real panel driver is wired up, a stub display is registered whose
 * flush callback discards pixels, so the UI still runs end to end.
 */
esp_err_t display_init(lv_display_t **out_disp);
