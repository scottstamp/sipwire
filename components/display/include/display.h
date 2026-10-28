#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "lvgl.h"

/**
 * ILI9341 (SPI) + XPT2046 (shared SPI bus) + PWM backlight, wired to LVGL through
 * esp_lvgl_port. Everything (pins, resolution, orientation, colour order) comes from
 * board_cfg(); settings supply backlight level / timeout / touch calibration.
 *
 * Call after board_init(), settings_init() and esp_event_loop_create_default().
 */

/**
 * Bring up LCD, touch and backlight, start the LVGL port (task + tick) and register the
 * display (and touch input, when board_cfg()->touch.present).
 *
 * The backlight stays off until the first frame has been flushed, then switches on at the
 * settings level. The returned display has the logical size of the panel after
 * swap_xy (e.g. 320x240 for a 240x320 panel with swap_xy).
 */
esp_err_t display_init(lv_display_t **out_disp);

/** Set backlight brightness 0-100 % immediately (not persisted; the settings value is
 *  re-applied when settings change). Also counts as activity. No-op without a BL pin. */
void display_set_backlight(uint8_t pct);

/** Report user activity (key press, touch, incoming call...): restores the backlight from
 *  dim/off and restarts the backlight timeout. Cheap; safe from any task (not from ISR). */
void display_wake(void);

/**
 * Touch calibration (reserved). Calibration is read from settings.touch_cal; when valid,
 * x_min/x_max are the raw 12-bit XPT2046 values (0..4095) on the touch axis that runs along
 * screen X (after swap_xy), at screen x = 0 and x = width-1; likewise y_min/y_max for
 * screen Y. min > max is allowed (inverted axis). When valid, the board's touch mirror
 * flags are not applied (calibration already encodes direction).
 *
 * Returns ESP_ERR_NOT_SUPPORTED: the interactive procedure is not implemented yet.
 */
esp_err_t display_touch_calibrate_start(void);
