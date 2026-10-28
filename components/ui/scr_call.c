/* In-call screen (ACTIVE / HELD): timer, mute / speaker / hold / volume,
 * DTMF keypad and hang up. The model is the single source of truth: toggles
 * send the opposite of what the model says and are redrawn on the next
 * snapshot. */

#include "ui_priv.h"

#include <stdio.h>
#include <string.h>

#define ECHO_MAX 9

typedef struct {
    lv_obj_t *who;
    lv_obj_t *sub;
    lv_obj_t *timer;
    lv_obj_t *hold_lbl; /* "On hold" under the timer (normal mode) */
    lv_obj_t *side;     /* right hand side of the timer row (keypad mode) */

    lv_obj_t *grid; /* container of the control buttons (normal mode) */
    lv_obj_t *mute;
    lv_obj_t *speaker;
    lv_obj_t *keypad_btn;
    lv_obj_t *hold;
    lv_obj_t *vol_val;

    lv_obj_t *pad; /* DTMF keypad (keypad mode) */
    lv_obj_t *back;
    lv_obj_t *hangup;

    bool pad_mode;
    char echo[ECHO_MAX + 1];
    char rname[64];
    char rnum[64];
    bool names_valid;
} call_t;

static call_t *s_c;

static void on_delete(lv_event_t *e)
{
    call_t *c = lv_event_get_user_data(e);
    if (s_c == c) {
        s_c = NULL;
    }
    lv_free(c);
}

static void set_checked(lv_obj_t *btn, bool on)
{
    if (on) {
        lv_obj_add_state(btn, LV_STATE_CHECKED);
    } else {
        lv_obj_remove_state(btn, LV_STATE_CHECKED);
    }
}

static void texts_refresh(call_t *c, const ui_model_t *m)
{
    const char *main_txt, *sub_txt;
    ui_remote_texts(m, &main_txt, &sub_txt);
    ui_fit_set(c->who, main_txt, c->pad_mode ? &lv_font_montserrat_24 : &lv_font_montserrat_32,
               !m->remote_name[0]);
    ui_fit_set(c->sub, sub_txt, &lv_font_montserrat_14, true);
}

static void side_refresh(call_t *c, const ui_model_t *m)
{
    if (m->call == UI_CALL_HELD) {
        lv_label_set_text(c->side, "On hold");
        lv_obj_set_style_text_color(c->side, UI_COL_WARN, 0);
    } else {
        lv_label_set_text(c->side, c->echo);
        lv_obj_set_style_text_color(c->side, UI_COL_BLUE, 0);
    }
}

/* Re-arrange the screen for the plain controls or the DTMF keypad. */
static void apply_mode(call_t *c)
{
    const ui_model_t *m = ui_model();
    bool kp = c->pad_mode;

    lv_obj_set_hidden(c->grid, kp);
    lv_obj_set_hidden(c->hold_lbl, kp);
    lv_obj_set_hidden(c->sub, kp);
    lv_obj_set_hidden(c->pad, !kp);
    lv_obj_set_hidden(c->back, !kp);
    lv_obj_set_hidden(c->side, !kp);

    lv_obj_set_height(c->who, kp ? 26 : 34);
    lv_obj_set_style_text_font(c->timer, kp ? &lv_font_montserrat_24 : &lv_font_montserrat_32, 0);
    if (kp) {
        lv_obj_set_width(c->timer, 110);
        lv_obj_set_style_text_align(c->timer, LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_set_pos(c->timer, 12, 50);
        lv_obj_set_pos(c->hangup, 72, 270);
        lv_obj_set_size(c->hangup, 162, 46);
    } else {
        lv_obj_set_width(c->timer, UI_W);
        lv_obj_set_style_text_align(c->timer, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(c->timer, 0, 78);
        lv_obj_set_pos(c->hangup, 6, 270);
        lv_obj_set_size(c->hangup, 228, 46);
    }
    texts_refresh(c, m);
    side_refresh(c, m);
}

static void on_mute(lv_event_t *e)
{
    (void)e;
    ui_act_set_mute(!ui_model()->muted);
}

static void on_speaker(lv_event_t *e)
{
    (void)e;
    ui_act_set_speaker(!ui_model()->speaker);
}

static void on_hold(lv_event_t *e)
{
    (void)e;
    ui_act_hold(ui_model()->call != UI_CALL_HELD);
}

static void on_vol(lv_event_t *e)
{
    int delta = (int)(intptr_t)lv_event_get_user_data(e);
    int v = (int)ui_model()->volume + delta;
    if (v < 0) {
        v = 0;
    }
    if (v > 100) {
        v = 100;
    }
    ui_act_set_volume((uint8_t)v);
}

static void send_dtmf(call_t *c, char digit)
{
    size_t n = strlen(c->echo);
    if (n >= ECHO_MAX) {
        memmove(c->echo, c->echo + 1, n); /* keep the most recent digits */
        n--;
    }
    c->echo[n] = digit;
    c->echo[n + 1] = '\0';
    ui_act_dtmf(digit);
    if (c->pad_mode) {
        side_refresh(c, ui_model());
    }
}

static void on_pad_key(char key, bool long_press, void *user)
{
    (void)long_press;
    send_dtmf(user, key);
}

static void on_toggle_pad(lv_event_t *e)
{
    call_t *c = lv_event_get_user_data(e);
    c->pad_mode = !c->pad_mode;
    apply_mode(c);
}

static void on_hangup(lv_event_t *e)
{
    (void)e;
    ui_act_hangup();
}

static lv_obj_t *vol_group_create(lv_obj_t *parent, call_t *c, int32_t x, int32_t y, int32_t w,
                                  int32_t h)
{
    lv_obj_t *g = ui_box_create(parent, x, y, w, h);
    lv_obj_set_style_bg_color(g, UI_COL_BAR, 0);
    lv_obj_set_style_bg_opa(g, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(g, 10, 0);

    lv_obj_t *minus = ui_btn_create(g, UI_BTN_NEUTRAL, LV_SYMBOL_MINUS, 0, 0, 46, h);
    lv_obj_add_event_cb(minus, on_vol, LV_EVENT_CLICKED, (void *)(intptr_t)-10);
    lv_obj_t *plus = ui_btn_create(g, UI_BTN_NEUTRAL, LV_SYMBOL_PLUS, w - 46, 0, 46, h);
    lv_obj_add_event_cb(plus, on_vol, LV_EVENT_CLICKED, (void *)(intptr_t)10);

    lv_obj_t *mid = ui_box_create(g, 46, 0, w - 92, h);
    lv_obj_set_flex_flow(mid, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(mid, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    c->vol_val = ui_label_create(mid, &lv_font_montserrat_24, UI_COL_TEXT, "");
    ui_label_create(mid, &lv_font_montserrat_14, UI_COL_DIM, "Volume");
    return g;
}

static lv_obj_t *call_create(const char *arg)
{
    (void)arg;
    call_t *c = lv_zalloc(sizeof(*c));
    if (!c) {
        return NULL;
    }
    lv_obj_t *scr = ui_screen_create();
    lv_obj_add_event_cb(scr, on_delete, LV_EVENT_DELETE, c);
    ui_statusbar_create(scr);

    c->who = ui_fit_create(scr, 6, 24, 228, 34, UI_COL_TEXT);
    c->sub = ui_fit_create(scr, 6, 58, 228, 18, UI_COL_DIM);

    c->timer = ui_label_create(scr, &lv_font_montserrat_32, UI_COL_TEXT, "00:00");
    c->hold_lbl = ui_label_create(scr, &lv_font_montserrat_14, UI_COL_WARN, "");
    lv_obj_set_width(c->hold_lbl, UI_W);
    lv_obj_set_style_text_align(c->hold_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(c->hold_lbl, 0, 118);

    c->side = ui_label_create(scr, &lv_font_montserrat_24, UI_COL_BLUE, "");
    lv_obj_set_width(c->side, 112);
    lv_obj_set_style_text_align(c->side, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(c->side, 122, 50);

    /* normal mode: 3 x 2 control grid */
    c->grid = ui_box_create(scr, 0, 138, UI_W, 124);
    const lv_font_t *f24 = &lv_font_montserrat_24;
    const lv_font_t *f14 = &lv_font_montserrat_14;
    c->mute = ui_icon_btn_create(c->grid, UI_BTN_NEUTRAL, LV_SYMBOL_MUTE, f24, 0, "Mute", f14, 6,
                                 0, 72, 58);
    lv_obj_add_event_cb(c->mute, on_mute, LV_EVENT_CLICKED, NULL);
    c->speaker = ui_icon_btn_create(c->grid, UI_BTN_NEUTRAL, LV_SYMBOL_VOLUME_MAX, f24, 0,
                                    "Speaker", f14, 84, 0, 72, 58);
    lv_obj_add_event_cb(c->speaker, on_speaker, LV_EVENT_CLICKED, NULL);
    c->keypad_btn = ui_icon_btn_create(c->grid, UI_BTN_NEUTRAL, LV_SYMBOL_KEYBOARD, f24, 0,
                                       "Keypad", f14, 162, 0, 72, 58);
    lv_obj_add_event_cb(c->keypad_btn, on_toggle_pad, LV_EVENT_CLICKED, c);
    c->hold = ui_icon_btn_create(c->grid, UI_BTN_NEUTRAL, LV_SYMBOL_PAUSE, f24, 0, "Hold", f14, 6,
                                 64, 72, 58);
    lv_obj_add_event_cb(c->hold, on_hold, LV_EVENT_CLICKED, NULL);
    vol_group_create(c->grid, c, 84, 64, 150, 58);

    /* keypad mode */
    c->pad = ui_keypad_create(scr, 6, 78, 228, 188, on_pad_key, c);
    lv_obj_set_style_pad_row(c->pad, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_column(c->pad, 6, LV_PART_MAIN);
    c->back = ui_btn_create(scr, UI_BTN_NEUTRAL, LV_SYMBOL_LEFT, 6, 270, 60, 46);
    lv_obj_add_event_cb(c->back, on_toggle_pad, LV_EVENT_CLICKED, c);

    c->hangup = ui_hangup_btn_create(scr, "Hang up", 6, 270, 228, 46);
    lv_obj_add_event_cb(c->hangup, on_hangup, LV_EVENT_CLICKED, NULL);

    s_c = c;
    apply_mode(c);
    return scr;
}

static void call_update(const ui_model_t *m)
{
    call_t *c = s_c;
    if (!c) {
        return;
    }

    if (!c->names_valid || strcmp(c->rname, m->remote_name) != 0 ||
        strcmp(c->rnum, m->remote_number) != 0) {
        snprintf(c->rname, sizeof(c->rname), "%s", m->remote_name);
        snprintf(c->rnum, sizeof(c->rnum), "%s", m->remote_number);
        c->names_valid = true;
        texts_refresh(c, m);
    }

    char t[16];
    bool held = m->call == UI_CALL_HELD;
    ui_fmt_duration(t, sizeof(t), m->call_seconds);
    lv_label_set_text(c->timer, t);
    lv_obj_set_style_text_color(c->timer, held ? UI_COL_WARN : UI_COL_TEXT, 0);
    lv_label_set_text(c->hold_lbl, held ? "On hold" : "");
    side_refresh(c, m);

    set_checked(c->mute, m->muted);
    set_checked(c->speaker, m->speaker);
    set_checked(c->hold, held);
    lv_label_set_text(lv_obj_get_child(c->mute, 1), m->muted ? "Muted" : "Mute");
    lv_label_set_text(lv_obj_get_child(c->hold, 0), held ? LV_SYMBOL_PLAY : LV_SYMBOL_PAUSE);
    lv_label_set_text(lv_obj_get_child(c->hold, 1), held ? "Resume" : "Hold");
    lv_label_set_text_fmt(c->vol_val, "%u", (unsigned)m->volume);
}

static void call_key(char key, ui_key_action_t action)
{
    call_t *c = s_c;
    if (!c) {
        return;
    }
    if (action == UI_KEY_PRESS && ui_is_dial_key(key)) {
        send_dtmf(c, key);
    } else if (action == UI_KEY_LONG && key == '*') {
        ui_act_hangup();
    }
}

const ui_screen_ops_t ui_ops_call = {call_create, call_update, call_key};
