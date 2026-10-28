/* Touch calibration: four crosshairs (TL, TR, BR, BL), then a test stage.
 *
 * While this screen is open the device keeps touches away from LVGL and feeds
 * raw samples to ui_touch_raw() (ui_actions_t.touch_cal_begin/end), so nothing
 * here can be clicked: the test stage is driven by the physical keys
 * (# save, 0 retry, * cancel) or a 1.5 s touch-hold on the Save box, whose
 * position is checked against the raw samples mapped with the *new*
 * calibration. The math lives in ui_touch_cal.c.
 *
 * Calibration is ended (touch_cal_end) exactly once on every way out: save,
 * cancel, 30 s without input, or the router replacing this screen (leave). */

#include "ui_priv.h"
#include "ui_touch_cal.h"

#include <stdio.h>
#include <string.h>

#define INSET 20
#define CROSS 33                 /* crosshair box size (odd: one centre pixel) */
#define IDLE_MS 30000
#define FAIL_MS 2000
#define HOLD_MS 1500
#define SETTLE_MS 350            /* ignore samples after an accepted point */
#define TICK_MS 100

#define SAVE_X 50
#define SAVE_Y 222
#define SAVE_W 140
#define SAVE_H 56
#define DOT 12

typedef enum { ST_TARGET, ST_FAILED, ST_TEST } cal_state_t;

typedef struct {
    lv_obj_t *cross, *title, *info, *progress, *hint, *dot, *save_box;
    lv_timer_t *timer;

    cal_state_t st;
    int idx;                     /* current target, ST_TARGET */
    ui_tcal_raw_t pts[UI_TCAL_POINTS];
    ui_tcal_cap_t cap;
    ui_touch_cal_t cal;          /* valid in ST_TEST */
    bool active;                 /* begin sent, end not yet */

    uint32_t last_input;         /* lv_tick of the last sample / key */
    uint32_t state_since;
    uint32_t ignore_until;
    uint32_t hold_since;         /* 0 = not holding the Save box */
} cal_t;

static cal_t *s_c;

static void on_delete(lv_event_t *e)
{
    cal_t *c = lv_event_get_user_data(e);
    if (c->active) { /* never leave the hook on, whatever deleted us */
        c->active = false;
        ui_act_touch_cal_end(NULL);
    }
    if (c->timer) {
        lv_timer_delete(c->timer);
    }
    if (s_c == c) {
        s_c = NULL;
    }
    lv_free(c);
}

/* Ends calibration once; `cal` NULL = cancelled. */
static void finish(cal_t *c, const ui_touch_cal_t *cal)
{
    if (c->active) {
        c->active = false;
        ui_act_touch_cal_end(cal);
    }
}

static void leave_to_settings(cal_t *c, const ui_touch_cal_t *cal, const char *toast)
{
    finish(c, cal);
    if (toast) {
        ui_toast(toast);
    }
    ui_goto(UI_SCR_SETTINGS, NULL);
}

/* ---- widgets --------------------------------------------------------------- */

static lv_obj_t *solid(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h, lv_color_t col)
{
    lv_obj_t *o = ui_box_create(parent, x, y, w, h);
    lv_obj_set_style_bg_color(o, col, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    return o;
}

static lv_obj_t *crosshair_create(lv_obj_t *parent)
{
    lv_obj_t *c = ui_box_create(parent, 0, 0, CROSS, CROSS);
    solid(c, 0, CROSS / 2 - 1, CROSS, 3, UI_COL_WARN);
    solid(c, CROSS / 2 - 1, 0, 3, CROSS, UI_COL_WARN);
    lv_obj_t *ring = ui_box_create(c, 7, 7, CROSS - 14, CROSS - 14);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(ring, 2, 0);
    lv_obj_set_style_border_color(ring, UI_COL_TEXT, 0);
    lv_obj_set_style_bg_color(ring, UI_COL_BG, 0);
    lv_obj_set_style_bg_opa(ring, LV_OPA_COVER, 0);
    solid(c, CROSS / 2 - 1, CROSS / 2 - 1, 3, 3, UI_COL_TEXT); /* centre pixel, on top */
    return c;
}

static lv_obj_t *wrapped_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t col, int32_t y)
{
    lv_obj_t *l = ui_label_create(parent, font, col, "");
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(l, UI_W - 40);
    lv_obj_set_pos(l, 20, y);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    return l;
}

/* ---- state ------------------------------------------------------------------ */

static void show_target(cal_t *c)
{
    int32_t x, y;
    ui_tcal_target(c->idx, UI_W, UI_H, INSET, &x, &y);
    lv_obj_set_pos(c->cross, x - CROSS / 2, y - CROSS / 2);
    lv_obj_set_hidden(c->cross, false);
    lv_label_set_text_fmt(c->progress, "Point %d of %d", c->idx + 1, UI_TCAL_POINTS);
}

static void start_targets(cal_t *c)
{
    c->st = ST_TARGET;
    c->idx = 0;
    c->hold_since = 0;
    ui_tcal_cap_reset(&c->cap);
    c->state_since = lv_tick_get();
    c->ignore_until = c->state_since + SETTLE_MS;

    lv_label_set_text(c->title, "Calibrate touch");
    lv_label_set_text(c->info, "Press and hold each cross until it moves on, then lift.");
    lv_obj_set_style_text_color(c->info, UI_COL_DIM, 0);
    lv_label_set_text(c->hint, "* Cancel");
    lv_obj_set_hidden(c->dot, true);
    lv_obj_set_hidden(c->save_box, true);
    show_target(c);
}

static void start_failed(cal_t *c)
{
    c->st = ST_FAILED;
    c->state_since = lv_tick_get();
    lv_obj_set_hidden(c->cross, true);
    lv_label_set_text(c->progress, "");
    lv_label_set_text(c->info, "Calibration failed, try again");
    lv_obj_set_style_text_color(c->info, UI_COL_BAD, 0);
}

static void start_test(cal_t *c)
{
    c->st = ST_TEST;
    c->hold_since = 0;
    c->state_since = lv_tick_get();
    lv_obj_set_hidden(c->cross, true);
    lv_label_set_text(c->title, "Test");
    lv_label_set_text(c->progress, "");
    lv_label_set_text(c->info, "Tap anywhere to test");
    lv_obj_set_style_text_color(c->info, UI_COL_TEXT, 0);
    lv_label_set_text(c->hint, "0 Retry    * Cancel");
    lv_obj_set_hidden(c->dot, true);
    lv_obj_set_hidden(c->save_box, false);
    lv_obj_set_style_bg_color(c->save_box, UI_COL_GREEN, 0);
}

static void target_done(cal_t *c, ui_tcal_raw_t pt)
{
    c->pts[c->idx] = pt;
    c->ignore_until = lv_tick_get() + SETTLE_MS;
    ui_tcal_cap_reset(&c->cap);
    if (++c->idx < UI_TCAL_POINTS) {
        show_target(c);
        return;
    }
    if (ui_tcal_compute(c->pts, UI_W, UI_H, INSET, &c->cal) == UI_TCAL_OK) {
        start_test(c);
    } else {
        start_failed(c);
    }
}

static bool in_save_box(int32_t x, int32_t y)
{
    return x >= SAVE_X && x < SAVE_X + SAVE_W && y >= SAVE_Y && y < SAVE_Y + SAVE_H;
}

static void do_save(cal_t *c)
{
    ui_touch_cal_t cal = c->cal;
    leave_to_settings(c, &cal, "Calibration saved");
}

/* ---- input -------------------------------------------------------------------- */

static void cal_touch_raw(int32_t ax, int32_t ay, bool pressed)
{
    cal_t *c = s_c;
    if (!c || !c->active) {
        return;
    }
    uint32_t now = lv_tick_get();
    if (pressed) {
        c->last_input = now;
    }

    if (c->st == ST_TARGET) {
        if ((int32_t)(now - c->ignore_until) < 0) {
            ui_tcal_cap_reset(&c->cap);
            return;
        }
        ui_tcal_raw_t pt;
        if (ui_tcal_cap_feed(&c->cap, ax, ay, pressed, &pt)) {
            target_done(c, pt);
        }
    } else if (c->st == ST_TEST) {
        if (!pressed) {
            c->hold_since = 0;
            lv_obj_set_style_bg_color(c->save_box, UI_COL_GREEN, 0);
            return;
        }
        int32_t sx = ui_tcal_map(ax, c->cal.x_min, c->cal.x_max, UI_W);
        int32_t sy = ui_tcal_map(ay, c->cal.y_min, c->cal.y_max, UI_H);
        lv_obj_set_pos(c->dot, sx - DOT / 2, sy - DOT / 2);
        lv_obj_set_hidden(c->dot, false);
        if (in_save_box(sx, sy)) {
            if (!c->hold_since) {
                c->hold_since = now ? now : 1;
                lv_obj_set_style_bg_color(c->save_box, UI_COL_OK, 0);
            }
        } else {
            c->hold_since = 0;
            lv_obj_set_style_bg_color(c->save_box, UI_COL_GREEN, 0);
        }
    }
}

static void cal_key(char key, ui_key_action_t action)
{
    cal_t *c = s_c;
    if (!c || action != UI_KEY_PRESS) {
        return;
    }
    c->last_input = lv_tick_get();
    if (key == '*') {
        leave_to_settings(c, NULL, NULL);
    } else if (c->st == ST_TEST && key == '#') {
        do_save(c);
    } else if (c->st == ST_TEST && key == '0') {
        start_targets(c);
    }
}

static void cal_timer(lv_timer_t *t)
{
    cal_t *c = lv_timer_get_user_data(t);
    uint32_t now = lv_tick_get();
    if (now - c->last_input >= IDLE_MS) {
        leave_to_settings(c, NULL, NULL);
    } else if (c->st == ST_FAILED && now - c->state_since >= FAIL_MS) {
        start_targets(c);
    } else if (c->st == ST_TEST && c->hold_since && now - c->hold_since >= HOLD_MS) {
        do_save(c);
    }
}

static void cal_leave(void)
{
    if (s_c) {
        finish(s_c, NULL);
    }
}

/* ---- screen ----------------------------------------------------------------- */

static lv_obj_t *cal_create(const char *arg)
{
    (void)arg;
    cal_t *c = lv_zalloc(sizeof(*c));
    if (!c) {
        return NULL;
    }
    lv_obj_t *scr = ui_screen_create();
    lv_obj_add_event_cb(scr, on_delete, LV_EVENT_DELETE, c);

    c->title = ui_label_create(scr, &lv_font_montserrat_24, UI_COL_TEXT, "");
    lv_obj_set_width(c->title, UI_W);
    lv_obj_set_style_text_align(c->title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(c->title, 0, 76);
    c->progress = ui_label_create(scr, &lv_font_montserrat_14, UI_COL_WARN, "");
    lv_obj_set_width(c->progress, UI_W);
    lv_obj_set_style_text_align(c->progress, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(c->progress, 0, 112);
    c->info = wrapped_label(scr, &lv_font_montserrat_14, UI_COL_DIM, 140);
    c->hint = ui_label_create(scr, &lv_font_montserrat_14, UI_COL_DIM, "");
    lv_obj_set_width(c->hint, UI_W);
    lv_obj_set_style_text_align(c->hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(c->hint, 0, 290);

    c->save_box = solid(scr, SAVE_X, SAVE_Y, SAVE_W, SAVE_H, UI_COL_GREEN);
    lv_obj_set_style_radius(c->save_box, 10, 0);
    lv_obj_t *sl = ui_label_create(c->save_box, &lv_font_montserrat_24, UI_COL_TEXT, "# Save");
    lv_obj_align(sl, LV_ALIGN_TOP_MID, 0, 6);
    lv_obj_t *ss = ui_label_create(c->save_box, &lv_font_montserrat_14, UI_COL_TEXT, "or hold here 1.5 s");
    lv_obj_align(ss, LV_ALIGN_BOTTOM_MID, 0, -6);

    c->cross = crosshair_create(scr);

    c->dot = solid(scr, 0, 0, DOT, DOT, UI_COL_RED);
    lv_obj_set_style_radius(c->dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(c->dot, 2, 0);
    lv_obj_set_style_border_color(c->dot, UI_COL_TEXT, 0);

    c->last_input = lv_tick_get();
    start_targets(c);
    c->timer = lv_timer_create(cal_timer, TICK_MS, c);

    s_c = c;
    c->active = true;
    ui_act_touch_cal_begin();
    return scr;
}

static void cal_update(const ui_model_t *m)
{
    (void)m; /* nothing model-driven; an incoming call replaces the screen (leave) */
}

const ui_screen_ops_t ui_ops_calibrate = {cal_create, cal_update, cal_key, cal_touch_raw, cal_leave};
