#pragma once

/* Private to components/ui: palette, shared styles and small widget helpers. */

#include <stdbool.h>
#include "lvgl.h"

#define UI_W 240
#define UI_H 320
#define UI_BAR_H 22 /* status bar height; screen content starts below */

/* ---- palette (dark, high contrast) ---- */
#define UI_COL_BG       lv_color_hex(0x0B0F14) /* screen background */
#define UI_COL_BAR      lv_color_hex(0x151B23) /* status bar / header */
#define UI_COL_SURFACE  lv_color_hex(0x232C38) /* buttons, fields */
#define UI_COL_SURFACE2 lv_color_hex(0x38465A) /* pressed / borders */
#define UI_COL_TEXT     lv_color_hex(0xF2F5F8)
#define UI_COL_DIM      lv_color_hex(0xA3AEBB)
#define UI_COL_BLUE     lv_color_hex(0x2F6FDE)
#define UI_COL_GREEN    lv_color_hex(0x178A45) /* fills (white text on top) */
#define UI_COL_RED      lv_color_hex(0xD6342F)
#define UI_COL_OK       lv_color_hex(0x3FB950) /* green as text on dark */
#define UI_COL_WARN     lv_color_hex(0xF0B429) /* amber */
#define UI_COL_BAD      lv_color_hex(0xFF6B64) /* red as text on dark */

typedef enum {
    UI_BTN_NEUTRAL,
    UI_BTN_GREEN,
    UI_BTN_RED,
    UI_BTN_BLUE,
    UI_BTN_KINDS
} ui_btn_kind_t;

void ui_theme_init(void);

/** Empty full-screen object with the dark background, not scrollable. */
lv_obj_t *ui_screen_create(void);

/** Transparent, non-scrollable, non-clickable container. */
lv_obj_t *ui_box_create(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h);

lv_obj_t *ui_label_create(lv_obj_t *parent, const lv_font_t *font, lv_color_t color,
                          const char *text);

/** Button with one centred label (24 pt) or none when text is NULL. */
lv_obj_t *ui_btn_create(lv_obj_t *parent, ui_btn_kind_t kind, const char *text,
                        int32_t x, int32_t y, int32_t w, int32_t h);

/** Button with an icon above a caption. Icon label is child 0, caption child 1.
 *  `rot` rotates the icon in 0.1 degree units (hang-up handset). */
lv_obj_t *ui_icon_btn_create(lv_obj_t *parent, ui_btn_kind_t kind, const char *icon,
                             const lv_font_t *icon_font, int32_t rot, const char *caption,
                             const lv_font_t *caption_font, int32_t x, int32_t y, int32_t w,
                             int32_t h);

/** Red wide hang-up button: rotated handset + caption in one row. */
lv_obj_t *ui_hangup_btn_create(lv_obj_t *parent, const char *caption, int32_t x, int32_t y,
                               int32_t w, int32_t h);

/** Round avatar with a centred symbol. */
lv_obj_t *ui_avatar_create(lv_obj_t *parent, const char *symbol, lv_color_t color, int32_t y);

/** Header strip (back button + title). Returns the back button. */
lv_obj_t *ui_header_create(lv_obj_t *parent, const char *title, int32_t y, lv_obj_t **title_label);
#define UI_HEADER_H 48

/** Single-line label that shrinks its font until `txt` fits the width.
 *  Fonts tried: 40, 32, 24, 14 starting at `start`. With `tail` an
 *  over-long text keeps its end, prefixed with "...". */
lv_obj_t *ui_fit_create(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h,
                        lv_color_t color);
void ui_fit_set(lv_obj_t *fit, const char *txt, const lv_font_t *start, bool tail);
void ui_fit_set_color(lv_obj_t *fit, lv_color_t color);

/** 3x4 phone keypad (button matrix). `cb` runs on release; `long_press` is
 *  set for a held '0' (touch equivalent of the long-0 '+'). */
typedef void (*ui_keypad_cb_t)(char key, bool long_press, void *user);
lv_obj_t *ui_keypad_create(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h,
                           ui_keypad_cb_t cb, void *user);

/** Apply the dark text field style (textarea). */
void ui_style_textarea(lv_obj_t *ta);

/** Format seconds as mm:ss (or h:mm:ss). */
void ui_fmt_duration(char *buf, size_t cap, uint32_t seconds);

/** Short-lived message at the bottom of the display. */
void ui_toast(const char *text);
