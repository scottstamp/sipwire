/*
 * LVGL configuration for the web simulator (sim/web).
 *
 * Keep the values that affect rendering in sync with the firmware's
 * sdkconfig.defaults (CONFIG_LV_*): colour depth, fonts, log level.
 * Everything not listed here takes the lv_conf_internal.h default, exactly
 * like the firmware build does.
 */
#ifndef LV_CONF_H
#define LV_CONF_H

/* ---- mirrored from sdkconfig.defaults ---- */
/* sdkconfig.defaults says CONFIG_LV_COLOR_DEPTH_16, which LVGL 9.6 deprecates
 * in favour of LV_COLOR_FORMAT_DEFAULT (same meaning: RGB565). */
#define LV_COLOR_FORMAT_DEFAULT LV_COLOR_FORMAT_RGB565

#define LV_USE_LOG 1
#define LV_LOG_LEVEL LV_LOG_LEVEL_WARN
#define LV_LOG_PRINTF 1            /* -> printf -> browser console */

#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_24 1
#define LV_FONT_DEFAULT &lv_font_montserrat_14

#define LV_USE_PERF_MONITOR 0

/* ---- simulator only ---- */
#define LV_USE_SDL 1
#define LV_SDL_INCLUDE_PATH <SDL2/SDL.h>
#define LV_SDL_RENDER_MODE LV_DISPLAY_RENDER_MODE_DIRECT
#define LV_SDL_BUF_COUNT 1
#define LV_SDL_ACCELERATED 1
#define LV_SDL_FULLSCREEN 0
#define LV_SDL_DIRECT_EXIT 0

#endif /* LV_CONF_H */
