#pragma once

#include <stdbool.h>
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
 *  dim/off and restarts the backlight timeout. Safe from any task (not from ISR). Cheap
 *  while the backlight is on; from off it takes the LVGL lock to wake the panel
 *  (SIPWIRE_DISPLAY_PANEL_SLEEP: SLPOUT + 5 ms), so never call it holding a lock the LVGL
 *  task may wait for. The backlight itself lights 120 ms after SLPOUT; LVGL may draw the
 *  new screen meanwhile. */
void display_wake(void);

/** False while the backlight is off (and the panel asleep); dimmed counts as lit. A change
 *  is announced by DISPLAY_EVENT_BACKLIGHT. Any task. */
bool display_is_lit(void);

/**
 * Touch calibration (reserved). Calibration is read from settings.touch_cal; when valid,
 * x_min/x_max are the raw 12-bit XPT2046 values (0..4095) on the touch axis that runs along
 * screen X (after swap_xy), at screen x = 0 and x = width-1; likewise y_min/y_max for
 * screen Y. min > max is allowed (inverted axis). When valid, the board's touch mirror
 * flags are not applied (calibration already encodes direction).
 *
 * Returns ESP_ERR_NOT_SUPPORTED: the interactive procedure is the UI's calibration screen
 * (Settings -> Touch calibration), which uses display_touch_raw_hook() below and stores the
 * result in settings.touch_cal.
 */
esp_err_t display_touch_calibrate_start(void);

/**
 * Raw touch hook, used by the touch calibration screen. While a hook is set every touch
 * sample is delivered to it as raw 12-bit XPT2046 values after swap_xy but before mirror /
 * calibration (the same space as settings.touch_cal), plus one pressed=false call when the
 * finger lifts (ax/ay are 0 then). LVGL sees no touches meanwhile (always "released"), so
 * nothing underneath gets clicked; when the hook is cleared again while the finger is still
 * down, that touch is swallowed until it lifts.
 *
 * The hook runs in the LVGL indev read path: LVGL task, LVGL lock held. Keep it trivial.
 * Call display_touch_raw_hook() itself with the LVGL lock held too. NULL clears the hook.
 * No-op in the QEMU build (no touch).
 */
typedef void (*display_touch_raw_cb_t)(int32_t ax, int32_t ay, bool pressed, void *ctx);
void display_touch_raw_hook(display_touch_raw_cb_t cb, void *ctx);
