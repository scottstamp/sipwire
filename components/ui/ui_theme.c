#include "ui_theme.h"

#include <stdio.h>
#include <string.h>

/* ---- shared styles ------------------------------------------------------- */

static lv_style_t s_st_screen;
static lv_style_t s_st_btn[UI_BTN_KINDS];
static lv_style_t s_st_btn_pr[UI_BTN_KINDS];
static lv_style_t s_st_btn_chk;
static lv_style_t s_st_kp_main;
static lv_style_t s_st_kp_item;
static lv_style_t s_st_kp_item_pr;
static lv_style_t s_st_ta;
static lv_style_t s_st_ta_focus;
static lv_style_t s_st_ta_cursor;
static lv_style_t s_st_ta_cursor_on;
static lv_style_t s_st_ta_ph;
static lv_style_t s_st_toast;
static bool s_inited;

static const lv_font_t *const FIT_FONTS[] = {
    &lv_font_montserrat_40,
    &lv_font_montserrat_32,
    &lv_font_montserrat_24,
    &lv_font_montserrat_14,
};
#define FIT_FONT_COUNT (sizeof(FIT_FONTS) / sizeof(FIT_FONTS[0]))

void ui_theme_init(void)
{
    if (s_inited) {
        return;
    }
    s_inited = true;

    lv_style_init(&s_st_screen);
    lv_style_set_bg_color(&s_st_screen, UI_COL_BG);
    lv_style_set_bg_opa(&s_st_screen, LV_OPA_COVER);
    lv_style_set_text_color(&s_st_screen, UI_COL_TEXT);
    lv_style_set_pad_all(&s_st_screen, 0);
    lv_style_set_border_width(&s_st_screen, 0);
    lv_style_set_radius(&s_st_screen, 0);

    const lv_color_t fill[UI_BTN_KINDS] = {UI_COL_SURFACE, UI_COL_GREEN, UI_COL_RED, UI_COL_BLUE};
    for (int i = 0; i < UI_BTN_KINDS; i++) {
        lv_style_init(&s_st_btn[i]);
        lv_style_set_bg_color(&s_st_btn[i], fill[i]);
        lv_style_set_bg_opa(&s_st_btn[i], LV_OPA_COVER);
        lv_style_set_radius(&s_st_btn[i], 10);
        lv_style_set_text_color(&s_st_btn[i], UI_COL_TEXT);
        lv_style_set_pad_all(&s_st_btn[i], 0);

        lv_style_init(&s_st_btn_pr[i]);
        lv_style_set_bg_color(&s_st_btn_pr[i],
                              i == UI_BTN_NEUTRAL ? UI_COL_SURFACE2 : lv_color_darken(fill[i], 70));
    }
    lv_style_init(&s_st_btn_chk);
    lv_style_set_bg_color(&s_st_btn_chk, UI_COL_BLUE);

    /* keypad */
    lv_style_init(&s_st_kp_main);
    lv_style_set_bg_opa(&s_st_kp_main, LV_OPA_TRANSP);
    lv_style_set_border_width(&s_st_kp_main, 0);
    lv_style_set_pad_all(&s_st_kp_main, 0);
    lv_style_set_pad_row(&s_st_kp_main, 6);
    lv_style_set_pad_column(&s_st_kp_main, 6);

    lv_style_init(&s_st_kp_item);
    lv_style_set_bg_color(&s_st_kp_item, UI_COL_SURFACE);
    lv_style_set_bg_opa(&s_st_kp_item, LV_OPA_COVER);
    lv_style_set_radius(&s_st_kp_item, 10);
    lv_style_set_border_width(&s_st_kp_item, 0);
    lv_style_set_shadow_width(&s_st_kp_item, 0);
    lv_style_set_text_color(&s_st_kp_item, UI_COL_TEXT);
    lv_style_set_text_font(&s_st_kp_item, &lv_font_montserrat_24);

    lv_style_init(&s_st_kp_item_pr);
    lv_style_set_bg_color(&s_st_kp_item_pr, UI_COL_BLUE);

    /* text fields */
    lv_style_init(&s_st_ta);
    lv_style_set_bg_color(&s_st_ta, UI_COL_SURFACE);
    lv_style_set_bg_opa(&s_st_ta, LV_OPA_COVER);
    lv_style_set_border_color(&s_st_ta, UI_COL_SURFACE2);
    lv_style_set_border_width(&s_st_ta, 2);
    lv_style_set_radius(&s_st_ta, 8);
    lv_style_set_text_color(&s_st_ta, UI_COL_TEXT);
    lv_style_set_text_font(&s_st_ta, &lv_font_montserrat_24);
    lv_style_set_pad_hor(&s_st_ta, 8);
    lv_style_set_pad_ver(&s_st_ta, 5);
    lv_style_set_shadow_width(&s_st_ta, 0);

    lv_style_init(&s_st_ta_focus);
    lv_style_set_border_color(&s_st_ta_focus, UI_COL_BLUE);

    /* cursor: invisible until the field is being edited */
    lv_style_init(&s_st_ta_cursor);
    lv_style_set_bg_opa(&s_st_ta_cursor, LV_OPA_TRANSP);
    lv_style_set_border_width(&s_st_ta_cursor, 0);
    lv_style_init(&s_st_ta_cursor_on);
    lv_style_set_border_color(&s_st_ta_cursor_on, UI_COL_TEXT);
    lv_style_set_border_width(&s_st_ta_cursor_on, 2);
    lv_style_set_border_side(&s_st_ta_cursor_on, LV_BORDER_SIDE_LEFT);

    lv_style_init(&s_st_ta_ph);
    lv_style_set_text_color(&s_st_ta_ph, lv_color_hex(0x7D8996));
    lv_style_set_text_font(&s_st_ta_ph, &lv_font_montserrat_14);

    lv_style_init(&s_st_toast);
    lv_style_set_bg_color(&s_st_toast, UI_COL_SURFACE2);
    lv_style_set_bg_opa(&s_st_toast, LV_OPA_COVER);
    lv_style_set_radius(&s_st_toast, 8);
    lv_style_set_pad_hor(&s_st_toast, 14);
    lv_style_set_pad_ver(&s_st_toast, 8);
    lv_style_set_text_color(&s_st_toast, UI_COL_TEXT);
    lv_style_set_text_font(&s_st_toast, &lv_font_montserrat_14);
    lv_style_set_text_align(&s_st_toast, LV_TEXT_ALIGN_CENTER);
}

/* ---- basic building blocks ------------------------------------------------ */

lv_obj_t *ui_screen_create(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_remove_style_all(scr);
    lv_obj_add_style(scr, &s_st_screen, 0);
    lv_obj_set_scrollable(scr, false);
    return scr;
}

lv_obj_t *ui_box_create(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_scrollable(o, false);
    lv_obj_set_clickable(o, false);
    return o;
}

lv_obj_t *ui_label_create(lv_obj_t *parent, const lv_font_t *font, lv_color_t color,
                          const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_label_set_text(l, text ? text : "");
    return l;
}

bool ui_label_set_text_if_changed(lv_obj_t *label, const char *txt)
{
    if (txt == NULL) {
        txt = "";
    }
    if (lv_label_get_long_mode(label) != LV_LABEL_LONG_MODE_DOTS) {
        const char *cur = lv_label_get_text(label);
        if (cur != NULL && strcmp(cur, txt) == 0) {
            return false;
        }
    }
    lv_label_set_text(label, txt);
    return true;
}

void ui_style_set_color(lv_obj_t *obj, lv_style_prop_t prop, lv_color_t color,
                        lv_style_selector_t selector)
{
    lv_style_value_t v;
    if (lv_obj_get_local_style_prop(obj, prop, &v, selector) == LV_STYLE_RES_FOUND &&
        lv_color_eq(v.color, color)) {
        return;
    }
    lv_obj_set_local_style_prop(obj, prop, (lv_style_value_t){ .color = color }, selector);
}

void ui_style_set_num(lv_obj_t *obj, lv_style_prop_t prop, int32_t num, lv_style_selector_t selector)
{
    lv_style_value_t v;
    if (lv_obj_get_local_style_prop(obj, prop, &v, selector) == LV_STYLE_RES_FOUND && v.num == num) {
        return;
    }
    lv_obj_set_local_style_prop(obj, prop, (lv_style_value_t){ .num = num }, selector);
}

void ui_set_text_color(lv_obj_t *obj, lv_color_t color)
{
    ui_style_set_color(obj, LV_STYLE_TEXT_COLOR, color, 0);
}

static lv_obj_t *btn_base(lv_obj_t *parent, ui_btn_kind_t kind, int32_t x, int32_t y, int32_t w,
                          int32_t h)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_add_style(b, &s_st_btn[kind], 0);
    lv_obj_add_style(b, &s_st_btn_pr[kind], LV_STATE_PRESSED);
    lv_obj_add_style(b, &s_st_btn_chk, LV_STATE_CHECKED);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, w, h);
    lv_obj_set_scrollable(b, false);
    lv_obj_set_scroll_on_focus(b, false);
    return b;
}

lv_obj_t *ui_btn_create(lv_obj_t *parent, ui_btn_kind_t kind, const char *text, int32_t x,
                        int32_t y, int32_t w, int32_t h)
{
    lv_obj_t *b = btn_base(parent, kind, x, y, w, h);
    if (text) {
        lv_obj_t *l = ui_label_create(b, &lv_font_montserrat_24, UI_COL_TEXT, text);
        lv_obj_center(l);
    }
    return b;
}

lv_obj_t *ui_icon_btn_create(lv_obj_t *parent, ui_btn_kind_t kind, const char *icon,
                             const lv_font_t *icon_font, int32_t rot, const char *caption,
                             const lv_font_t *caption_font, int32_t x, int32_t y, int32_t w,
                             int32_t h)
{
    lv_obj_t *b = btn_base(parent, kind, x, y, w, h);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(b, 2, 0);
    lv_obj_t *ic = ui_label_create(b, icon_font, UI_COL_TEXT, icon);
    if (rot) {
        lv_obj_set_style_transform_pivot_x(ic, LV_PCT(50), 0);
        lv_obj_set_style_transform_pivot_y(ic, LV_PCT(50), 0);
        lv_obj_set_style_transform_rotation(ic, rot, 0);
    }
    ui_label_create(b, caption_font, UI_COL_TEXT, caption);
    return b;
}

lv_obj_t *ui_hangup_btn_create(lv_obj_t *parent, const char *caption, int32_t x, int32_t y,
                               int32_t w, int32_t h)
{
    lv_obj_t *b = btn_base(parent, UI_BTN_RED, x, y, w, h);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(b, 10, 0);
    lv_obj_t *ic = ui_label_create(b, &lv_font_montserrat_24, UI_COL_TEXT, LV_SYMBOL_CALL);
    lv_obj_set_style_transform_pivot_x(ic, LV_PCT(50), 0);
    lv_obj_set_style_transform_pivot_y(ic, LV_PCT(50), 0);
    lv_obj_set_style_transform_rotation(ic, 2250, 0);
    ui_label_create(b, &lv_font_montserrat_24, UI_COL_TEXT, caption);
    return b;
}

lv_obj_t *ui_avatar_create(lv_obj_t *parent, const char *symbol, lv_color_t color, int32_t y)
{
    lv_obj_t *av = ui_box_create(parent, 0, y, 84, 84);
    lv_obj_align(av, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_set_style_radius(av, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(av, UI_COL_SURFACE, 0);
    lv_obj_set_style_bg_opa(av, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(av, color, 0);
    lv_obj_set_style_border_width(av, 3, 0);
    lv_obj_t *l = ui_label_create(av, &lv_font_montserrat_40, color, symbol);
    lv_obj_center(l);
    return av;
}

lv_obj_t *ui_header_create(lv_obj_t *parent, const char *title, int32_t y, lv_obj_t **title_label)
{
    lv_obj_t *bar = ui_box_create(parent, 0, y, UI_W, UI_HEADER_H);
    lv_obj_set_style_bg_color(bar, UI_COL_BAR, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_t *back = ui_btn_create(bar, UI_BTN_NEUTRAL, LV_SYMBOL_LEFT, 2, 2, 46, 44);
    lv_obj_t *t = ui_label_create(bar, &lv_font_montserrat_24, UI_COL_TEXT, title);
    lv_obj_align(t, LV_ALIGN_LEFT_MID, 56, 0);
    if (title_label) {
        *title_label = t;
    }
    return back;
}

/* ---- fitted single-line label ------------------------------------------ */

static int32_t text_width(const lv_font_t *f, const char *t)
{
    int32_t w = 0;
    for (; *t; t++) {
        uint8_t c = (uint8_t)*t;
        if (c >= 0x80 && c < 0xC0) {
            continue; /* UTF-8 continuation byte */
        }
        if (c >= 0xC0) {
            c = '0'; /* approximate a multi-byte glyph by a digit */
        }
        uint8_t n = (uint8_t)t[1];
        w += lv_font_get_glyph_width(f, c, n >= 0x80 ? 0 : n);
    }
    return w;
}

/* Last arguments of ui_fit_set / ui_fit_set_fixed (the box's user data). The label is in DOTS
 * mode, which writes "..." into LVGL's copy of the text, so it cannot be compared with
 * lv_label_get_text. */
#define FIT_CACHE_TXT 72
typedef struct {
    const lv_font_t *font;      /* `start` or the fixed font; NULL = nothing cached */
    int32_t w;
    uint8_t mode;               /* FIT_MODE_* */
    char txt[FIT_CACHE_TXT];
} fit_cache_t;
enum { FIT_MODE_FIT, FIT_MODE_FIT_TAIL, FIT_MODE_FIXED };

static void fit_deleted(lv_event_t *e)
{
    lv_free(lv_event_get_user_data(e));
}

/* True when `fit` already shows exactly this; otherwise remembers it (if it fits the cache). */
static bool fit_unchanged(lv_obj_t *fit, const char *txt, const lv_font_t *font, uint8_t mode, int32_t w)
{
    fit_cache_t *fc = lv_obj_get_user_data(fit);
    if (fc == NULL) {
        return false;
    }
    if (fc->font == font && fc->mode == mode && fc->w == w && strcmp(fc->txt, txt) == 0) {
        return true;
    }
    size_t n = strlen(txt);
    if (n >= sizeof(fc->txt)) {
        fc->font = NULL;        /* too long to remember: always set */
        return false;
    }
    memcpy(fc->txt, txt, n + 1);
    fc->font = font;
    fc->mode = mode;
    fc->w = w;
    return false;
}

lv_obj_t *ui_fit_create(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h,
                        lv_color_t color)
{
    lv_obj_t *box = ui_box_create(parent, x, y, w, h);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *l = ui_label_create(box, &lv_font_montserrat_24, color, "");
    lv_obj_set_width(l, w);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    fit_cache_t *fc = lv_zalloc(sizeof(*fc));
    if (fc != NULL) {   /* without it every set applies, as before */
        lv_obj_set_user_data(box, fc);
        lv_obj_add_event_cb(box, fit_deleted, LV_EVENT_DELETE, fc);
    }
    return box;
}

void ui_fit_set_color(lv_obj_t *fit, lv_color_t color)
{
    ui_set_text_color(lv_obj_get_child(fit, 0), color);
}

void ui_fit_set(lv_obj_t *fit, const char *txt, const lv_font_t *start, bool tail)
{
    lv_obj_t *l = lv_obj_get_child(fit, 0);
    /* the style width, not the coords: those are only valid after a layout pass */
    int32_t w = lv_obj_get_style_width(fit, LV_PART_MAIN);
    if (fit_unchanged(fit, txt, start, tail ? FIT_MODE_FIT_TAIL : FIT_MODE_FIT, w)) {
        return;
    }
    size_t first = 0;
    for (size_t i = 0; i < FIT_FONT_COUNT; i++) {
        if (FIT_FONTS[i] == start) {
            first = i;
            break;
        }
    }
    /* A tail-trimmed text (a long number) stays readable: never below 24 pt. */
    size_t last = FIT_FONT_COUNT - 1;
    if (tail && first <= 2) {
        last = 2;
    }
    const lv_font_t *f = FIT_FONTS[first];
    for (size_t i = first; i <= last; i++) {
        f = FIT_FONTS[i];
        if (text_width(f, txt) <= w) {
            break;
        }
    }

    char shown[72];
    snprintf(shown, sizeof(shown), "%s", txt);
    if (tail && text_width(f, shown) > w) {
        size_t len = strlen(txt);
        for (size_t skip = 1; skip < len; skip++) {
            snprintf(shown, sizeof(shown), "...%s", txt + skip);
            if (text_width(f, shown) <= w) {
                break;
            }
        }
    }
    lv_obj_set_style_text_font(l, f, 0);
    /* one line only: DOTS mode would otherwise wrap onto further lines */
    lv_obj_set_height(l, lv_font_get_line_height(f));
    lv_label_set_text(l, shown);
}

void ui_fit_set_fixed(lv_obj_t *fit, const char *txt, const lv_font_t *font)
{
    if (fit_unchanged(fit, txt, font, FIT_MODE_FIXED, lv_obj_get_style_width(fit, LV_PART_MAIN))) {
        return;
    }
    lv_obj_t *l = lv_obj_get_child(fit, 0);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_height(l, lv_font_get_line_height(font));    /* DOTS mode: one line */
    lv_label_set_text(l, txt);
}

/* ---- keypad ------------------------------------------------------------- */

typedef struct {
    ui_keypad_cb_t cb;
    void *user;
    bool long_fired;
} keypad_t;

static const char *const KEYPAD_MAP[] = {"1", "2", "3", "\n", "4", "5", "6", "\n",
                                         "7", "8", "9", "\n", "*", "0", "#", ""};

static void keypad_event(lv_event_t *e)
{
    keypad_t *k = lv_event_get_user_data(e);
    lv_obj_t *m = lv_event_get_target_obj(e);
    switch (lv_event_get_code(e)) {
    case LV_EVENT_PRESSED:
        k->long_fired = false;
        break;
    case LV_EVENT_LONG_PRESSED:
        k->long_fired = true;
        break;
    case LV_EVENT_VALUE_CHANGED: {
        uint32_t id = lv_buttonmatrix_get_selected_button(m);
        if (id == LV_BUTTONMATRIX_BUTTON_NONE) {
            break;
        }
        const char *t = lv_buttonmatrix_get_button_text(m, id);
        if (t && t[0] && k->cb) {
            k->cb(t[0], k->long_fired, k->user);
        }
        k->long_fired = false;
        break;
    }
    case LV_EVENT_DELETE:
        lv_free(k);
        break;
    default:
        break;
    }
}

lv_obj_t *ui_keypad_create(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h,
                           ui_keypad_cb_t cb, void *user)
{
    lv_obj_t *m = lv_buttonmatrix_create(parent);
    lv_obj_remove_style_all(m);
    lv_obj_add_style(m, &s_st_kp_main, LV_PART_MAIN);
    lv_obj_add_style(m, &s_st_kp_item, LV_PART_ITEMS);
    lv_obj_add_style(m, &s_st_kp_item_pr, LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_pos(m, x, y);
    lv_obj_set_size(m, w, h);
    lv_buttonmatrix_set_map(m, KEYPAD_MAP);

    keypad_t *k = lv_malloc(sizeof(*k));
    if (k) {
        k->cb = cb;
        k->user = user;
        k->long_fired = false;
        lv_obj_add_event_cb(m, keypad_event, LV_EVENT_ALL, k);
    }
    return m;
}

/* ---- misc -------------------------------------------------------------- */

void ui_style_textarea(lv_obj_t *ta)
{
    lv_obj_add_style(ta, &s_st_ta, LV_PART_MAIN);
    lv_obj_add_style(ta, &s_st_ta_focus, LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_add_style(ta, &s_st_ta_cursor, LV_PART_CURSOR);
    lv_obj_add_style(ta, &s_st_ta_cursor_on, LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_obj_add_style(ta, &s_st_ta_ph, LV_PART_TEXTAREA_PLACEHOLDER);
}

void ui_style_dropdown(lv_obj_t *dd)
{
    lv_obj_set_style_bg_color(dd, UI_COL_SURFACE, 0);
    lv_obj_set_style_bg_opa(dd, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(dd, UI_COL_SURFACE2, 0);
    lv_obj_set_style_border_width(dd, 2, 0);
    lv_obj_set_style_radius(dd, 8, 0);
    lv_obj_set_style_text_color(dd, UI_COL_TEXT, 0);
    lv_obj_set_style_text_font(dd, &lv_font_montserrat_24, 0);
    lv_obj_set_style_pad_hor(dd, 8, 0);
    lv_obj_set_style_pad_ver(dd, 6, 0);
    lv_obj_set_style_shadow_width(dd, 0, 0);
    lv_obj_set_style_border_color(dd, UI_COL_BLUE, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_width(dd, 0, LV_STATE_FOCUS_KEY);

    /* The popup list lives on the screen, not inside the dropdown: style it separately.
     * Line space makes every option a 44 px touch target. */
    lv_obj_t *list = lv_dropdown_get_list(dd);
    lv_obj_set_style_bg_color(list, UI_COL_SURFACE, 0);
    lv_obj_set_style_bg_opa(list, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(list, UI_COL_BLUE, 0);
    lv_obj_set_style_border_width(list, 2, 0);
    lv_obj_set_style_radius(list, 8, 0);
    lv_obj_set_style_pad_hor(list, 8, 0);
    lv_obj_set_style_text_color(list, UI_COL_TEXT, 0);
    lv_obj_set_style_text_font(list, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_line_space(list, 16, 0);
    lv_obj_set_style_shadow_width(list, 0, 0);
    lv_obj_set_style_bg_color(list, UI_COL_BLUE, LV_PART_SELECTED | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(list, UI_COL_SURFACE2, LV_PART_SELECTED | LV_STATE_PRESSED);
    lv_obj_set_style_text_color(list, UI_COL_TEXT, LV_PART_SELECTED | LV_STATE_CHECKED);
}

void ui_fmt_duration(char *buf, size_t cap, uint32_t s)
{
    if (s >= 3600) {
        snprintf(buf, cap, "%u:%02u:%02u", (unsigned)(s / 3600), (unsigned)((s / 60) % 60),
                 (unsigned)(s % 60));
    } else {
        snprintf(buf, cap, "%02u:%02u", (unsigned)(s / 60), (unsigned)(s % 60));
    }
}

static lv_obj_t *s_toast;

static void toast_deleted(lv_event_t *e)
{
    if (s_toast == lv_event_get_target_obj(e)) {
        s_toast = NULL;
    }
}

void ui_toast(const char *text)
{
    ui_toast_for(text, 1800);
}

void ui_toast_for(const char *text, uint32_t ms)
{
    if (s_toast) {
        lv_obj_delete(s_toast);
        s_toast = NULL;
    }
    lv_obj_t *t = lv_label_create(lv_layer_top());
    lv_obj_add_style(t, &s_st_toast, 0);
    lv_label_set_text(t, text);
    /* A long text wraps at a fixed width (a content-sized label with max_width would size its
     * height for the unwrapped text and clip the last line). */
    lv_obj_update_layout(t);
    if (lv_obj_get_width(t) > UI_W - 16) {
        lv_obj_set_width(t, UI_W - 16);
    }
    lv_obj_align(t, LV_ALIGN_BOTTOM_MID, 0, -12);
    lv_obj_set_clickable(t, false);
    lv_obj_add_event_cb(t, toast_deleted, LV_EVENT_DELETE, NULL);
    lv_obj_delete_delayed(t, ms);
    s_toast = t;
}
