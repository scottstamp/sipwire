/* In-call screen (ACTIVE / HELD): timer, mute / speaker / hold / transfer / volume,
 * DTMF keypad and hang up. The model is the single source of truth: toggles
 * send the opposite of what the model says and are redrawn on the next
 * snapshot.
 *
 * Call features (see ui_model.h "call features"):
 *   - a waiting second call raises an overlay with Answer (hold + answer) / Reject;
 *   - a held second call shows a strip (name, "On hold") with a Swap button; the
 *     Hold and Transfer buttons are not offered then;
 *   - attended transfer: the foreground call is the consultation call, the original
 *     is the held strip, Complete / Cancel replace Hang up;
 *   - status line: transfer progress / failure, "On hold", or "Page / Intercom";
 *   - Transfer opens the target picker (scr_transfer.c). */

#include "ui_priv.h"

#include <stdio.h>
#include <string.h>

#define ECHO_MAX 9

typedef struct {
    lv_obj_t *who;
    lv_obj_t *sub;
    lv_obj_t *timer;
    lv_obj_t *hold_lbl; /* status line under the timer (normal mode) */
    lv_obj_t *side;     /* right hand side of the timer row (keypad mode) */

    lv_obj_t *grid; /* container of the control buttons (normal mode) */
    lv_obj_t *mute;
    lv_obj_t *speaker;
    lv_obj_t *keypad_btn;
    lv_obj_t *hold;
    lv_obj_t *xfer;
    lv_obj_t *vol;
    lv_obj_t *vol_minus;
    lv_obj_t *vol_plus;
    lv_obj_t *vol_val;

    lv_obj_t *pad; /* DTMF keypad (keypad mode) */
    lv_obj_t *back;
    lv_obj_t *hangup;
    lv_obj_t *complete; /* attended transfer: Complete / Cancel instead of Hang up */
    lv_obj_t *cancel;

    lv_obj_t *strip;     /* second (held) call: tag, name, Swap */
    lv_obj_t *strip_tag;
    lv_obj_t *strip_who;
    lv_obj_t *swap;

    lv_obj_t *wait;      /* call waiting overlay */
    lv_obj_t *wait_who;
    lv_obj_t *wait_sub;

    bool pad_mode;
    bool two;     /* a held background call exists, or an attended transfer is consulting */
    bool consult; /* attended transfer consultation */
    bool waiting;
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

static void set_enabled(lv_obj_t *btn, bool on)
{
    if (on) {
        lv_obj_remove_state(btn, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(btn, LV_STATE_DISABLED);
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

static void vol_layout(call_t *c, int32_t w, int32_t h)
{
    lv_obj_set_size(c->vol, w, h);
    lv_obj_set_size(c->vol_minus, 32, h);
    lv_obj_set_size(c->vol_plus, 32, h);
    lv_obj_set_x(c->vol_plus, w - 32);
    lv_obj_t *mid = lv_obj_get_parent(c->vol_val);
    lv_obj_set_pos(mid, 32, 0);
    lv_obj_set_size(mid, w - 64, h);
}

/* Place a control row cell. Columns: 6/64, 76/64, 146/88. */
static void cell(lv_obj_t *o, int col, int32_t y, int32_t h)
{
    static const int32_t X[3] = {6, 76, 146};
    static const int32_t W[3] = {64, 64, 88};
    lv_obj_set_pos(o, X[col], y);
    lv_obj_set_size(o, W[col], h);
}

/* Re-arrange the screen for the plain controls, the held-call strip or the DTMF keypad. */
static void apply_mode(call_t *c)
{
    const ui_model_t *m = ui_model();
    bool kp = c->pad_mode;
    bool two = c->two && !kp;
    int32_t bh = two ? 48 : 58;

    lv_obj_set_hidden(c->grid, kp);
    lv_obj_set_hidden(c->hold_lbl, kp || two);
    lv_obj_set_hidden(c->sub, kp);
    lv_obj_set_hidden(c->pad, !kp);
    lv_obj_set_hidden(c->back, !kp);
    lv_obj_set_hidden(c->side, !kp);
    lv_obj_set_hidden(c->strip, !two);

    lv_obj_set_pos(c->grid, 0, two ? 166 : 138);
    cell(c->mute, 0, 0, bh);
    cell(c->speaker, 1, 0, bh);
    cell(c->keypad_btn, 2, 0, bh);
    cell(c->hold, 0, bh + 6, bh);
    cell(c->xfer, 1, bh + 6, bh);
    cell(c->vol, 2, bh + 6, bh);
    vol_layout(c, 88, bh);
    lv_obj_set_hidden(c->hold, c->two);
    lv_obj_set_hidden(c->xfer, c->two);
    lv_obj_set_hidden(c->keypad_btn, c->consult);

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
    lv_obj_set_hidden(c->hangup, c->consult);
    lv_obj_set_hidden(c->complete, !c->consult);
    lv_obj_set_hidden(c->cancel, !c->consult);
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

static void on_transfer(lv_event_t *e)
{
    (void)e;
    ui_goto(UI_SCR_TRANSFER, NULL);
}

static void on_swap(lv_event_t *e)
{
    (void)e;
    ui_act_swap();
}

static void on_complete(lv_event_t *e)
{
    (void)e;
    ui_act_transfer_attended_complete();
}

static void on_cancel(lv_event_t *e)
{
    (void)e;
    ui_act_transfer_attended_cancel();
}

static void on_wait_answer(lv_event_t *e)
{
    (void)e;
    ui_act_answer_waiting();
}

static void on_wait_reject(lv_event_t *e)
{
    (void)e;
    ui_act_reject_waiting();
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

static lv_obj_t *vol_group_create(lv_obj_t *parent, call_t *c)
{
    lv_obj_t *g = ui_box_create(parent, 0, 0, 88, 58);
    lv_obj_set_style_bg_color(g, UI_COL_BAR, 0);
    lv_obj_set_style_bg_opa(g, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(g, 10, 0);
    c->vol = g;

    c->vol_minus = ui_btn_create(g, UI_BTN_NEUTRAL, LV_SYMBOL_MINUS, 0, 0, 32, 58);
    lv_obj_add_event_cb(c->vol_minus, on_vol, LV_EVENT_CLICKED, (void *)(intptr_t)-10);
    c->vol_plus = ui_btn_create(g, UI_BTN_NEUTRAL, LV_SYMBOL_PLUS, 56, 0, 32, 58);
    lv_obj_add_event_cb(c->vol_plus, on_vol, LV_EVENT_CLICKED, (void *)(intptr_t)10);
    for (lv_obj_t *b = c->vol_minus; b; b = (b == c->vol_minus) ? c->vol_plus : NULL) {
        lv_obj_set_style_text_font(lv_obj_get_child(b, 0), &lv_font_montserrat_14, 0);
    }

    lv_obj_t *mid = ui_box_create(g, 32, 0, 24, 58);
    lv_obj_set_flex_flow(mid, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(mid, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(mid, 0, 0);
    ui_label_create(mid, &lv_font_montserrat_14, UI_COL_DIM, "Vol");
    c->vol_val = ui_label_create(mid, &lv_font_montserrat_14, UI_COL_TEXT, "");
    return g;
}

static void strip_create(call_t *c, lv_obj_t *scr)
{
    c->strip = ui_box_create(scr, 6, 118, 228, 46);
    lv_obj_set_style_bg_color(c->strip, UI_COL_BAR, 0);
    lv_obj_set_style_bg_opa(c->strip, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(c->strip, 10, 0);
    c->strip_tag = ui_label_create(c->strip, &lv_font_montserrat_14, UI_COL_WARN, "");
    lv_obj_set_pos(c->strip_tag, 8, 3);
    c->strip_who = ui_fit_create(c->strip, 8, 17, 138, 28, UI_COL_TEXT);
    lv_obj_set_style_text_align(lv_obj_get_child(c->strip_who, 0), LV_TEXT_ALIGN_LEFT, 0);
    c->swap = ui_icon_btn_create(c->strip, UI_BTN_BLUE, LV_SYMBOL_REFRESH, &lv_font_montserrat_14,
                                 0, "Swap", &lv_font_montserrat_14, 150, 3, 76, 40);
    lv_obj_set_flex_flow(c->swap, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(c->swap, 6, 0);
    lv_obj_add_event_cb(c->swap, on_swap, LV_EVENT_CLICKED, NULL);
}

static void wait_create(call_t *c, lv_obj_t *scr)
{
    c->wait = ui_box_create(scr, 6, 84, 228, 176);
    lv_obj_set_style_bg_color(c->wait, UI_COL_SURFACE, 0);
    lv_obj_set_style_bg_opa(c->wait, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(c->wait, 12, 0);
    lv_obj_set_style_border_color(c->wait, UI_COL_WARN, 0);
    lv_obj_set_style_border_width(c->wait, 2, 0);
    lv_obj_t *tag = ui_label_create(c->wait, &lv_font_montserrat_14, UI_COL_WARN, "CALL WAITING");
    lv_obj_set_style_text_letter_space(tag, 2, 0);
    lv_obj_set_width(tag, 224);
    lv_obj_set_style_text_align(tag, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(tag, 0, 8);
    c->wait_who = ui_fit_create(c->wait, 6, 30, 212, 36, UI_COL_TEXT);
    c->wait_sub = ui_fit_create(c->wait, 6, 68, 212, 18, UI_COL_DIM);
    lv_obj_t *ans = ui_icon_btn_create(c->wait, UI_BTN_GREEN, LV_SYMBOL_CALL, &lv_font_montserrat_24,
                                       0, "Hold + answer", &lv_font_montserrat_14, 6, 98, 104, 66);
    lv_obj_add_event_cb(ans, on_wait_answer, LV_EVENT_CLICKED, NULL);
    lv_obj_t *rej = ui_icon_btn_create(c->wait, UI_BTN_RED, LV_SYMBOL_CALL, &lv_font_montserrat_24,
                                       2250, "Reject", &lv_font_montserrat_14, 114, 98, 104, 66);
    lv_obj_add_event_cb(rej, on_wait_reject, LV_EVENT_CLICKED, NULL);
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

    /* normal mode: 3 x 2 control grid (columns 64 / 64 / 88) */
    c->grid = ui_box_create(scr, 0, 138, UI_W, 124);
    const lv_font_t *f24 = &lv_font_montserrat_24;
    const lv_font_t *f14 = &lv_font_montserrat_14;
    c->mute = ui_icon_btn_create(c->grid, UI_BTN_NEUTRAL, LV_SYMBOL_MUTE, f24, 0, "Mute", f14, 6,
                                 0, 64, 58);
    lv_obj_add_event_cb(c->mute, on_mute, LV_EVENT_CLICKED, NULL);
    c->speaker = ui_icon_btn_create(c->grid, UI_BTN_NEUTRAL, LV_SYMBOL_VOLUME_MAX, f24, 0,
                                    "Speaker", f14, 76, 0, 64, 58);
    lv_obj_add_event_cb(c->speaker, on_speaker, LV_EVENT_CLICKED, NULL);
    c->keypad_btn = ui_icon_btn_create(c->grid, UI_BTN_NEUTRAL, LV_SYMBOL_KEYBOARD, f24, 0,
                                       "Keypad", f14, 146, 0, 88, 58);
    lv_obj_add_event_cb(c->keypad_btn, on_toggle_pad, LV_EVENT_CLICKED, c);
    c->hold = ui_icon_btn_create(c->grid, UI_BTN_NEUTRAL, LV_SYMBOL_PAUSE, f24, 0, "Hold", f14, 6,
                                 64, 64, 58);
    lv_obj_add_event_cb(c->hold, on_hold, LV_EVENT_CLICKED, NULL);
    c->xfer = ui_icon_btn_create(c->grid, UI_BTN_NEUTRAL, LV_SYMBOL_SHUFFLE, f24, 0, "Transfer",
                                 f14, 76, 64, 64, 58);
    lv_obj_add_event_cb(c->xfer, on_transfer, LV_EVENT_CLICKED, NULL);
    vol_group_create(c->grid, c);
    lv_obj_set_pos(c->vol, 146, 64);

    /* keypad mode */
    c->pad = ui_keypad_create(scr, 6, 78, 228, 188, on_pad_key, c);
    lv_obj_set_style_pad_row(c->pad, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_column(c->pad, 6, LV_PART_MAIN);
    c->back = ui_btn_create(scr, UI_BTN_NEUTRAL, LV_SYMBOL_LEFT, 6, 270, 60, 46);
    lv_obj_add_event_cb(c->back, on_toggle_pad, LV_EVENT_CLICKED, c);

    c->hangup = ui_hangup_btn_create(scr, "Hang up", 6, 270, 228, 46);
    lv_obj_add_event_cb(c->hangup, on_hangup, LV_EVENT_CLICKED, NULL);

    /* call features: consultation buttons, held-call strip, waiting overlay (on top) */
    c->complete = ui_btn_create(scr, UI_BTN_GREEN, "Complete", 6, 270, 126, 46);
    lv_obj_add_event_cb(c->complete, on_complete, LV_EVENT_CLICKED, NULL);
    lv_obj_set_style_opa(c->complete, LV_OPA_50, LV_STATE_DISABLED);
    c->cancel = ui_btn_create(scr, UI_BTN_RED, "Cancel", 138, 270, 96, 46);
    lv_obj_add_event_cb(c->cancel, on_cancel, LV_EVENT_CLICKED, NULL);
    lv_obj_set_style_text_font(lv_obj_get_child(c->complete, 0), &lv_font_montserrat_24, 0);
    strip_create(c, scr);
    wait_create(c, scr);
    lv_obj_set_hidden(c->wait, true);

    s_c = c;
    apply_mode(c);
    return scr;
}

/* Fill the status line under the timer: transfer progress, hold or page. */
static void status_refresh(call_t *c, const ui_model_t *m)
{
    char buf[80];
    bool bad = false;
    const char *t = ui_xfer_text(m, buf, sizeof(buf), &bad);
    lv_color_t col = bad ? UI_COL_BAD : UI_COL_BLUE;
    if (!t && m->call == UI_CALL_HELD) {
        t = "On hold";
        col = UI_COL_WARN;
    }
    if (!t && m->auto_answered) {
        snprintf(buf, sizeof(buf), "Page / Intercom - speaker %s", m->speaker ? "on" : "off");
        t = buf;
        col = UI_COL_OK;
    }
    lv_label_set_text(c->hold_lbl, t ? t : "");
    lv_obj_set_style_text_color(c->hold_lbl, col, 0);
}

static void call_update(const ui_model_t *m)
{
    call_t *c = s_c;
    if (!c) {
        return;
    }

    bool consult = m->xfer == UI_XFER_CONSULT && m->xfer_attended;
    bool two = consult || m->second_call == UI_CALL_HELD;
    bool waiting = m->second_call == UI_CALL_RINGING_IN;
    bool relayout = two != c->two || consult != c->consult;
    c->two = two;
    c->consult = consult;
    if (consult && c->pad_mode) {
        c->pad_mode = false;
        relayout = true;
    }
    if (relayout) {
        apply_mode(c);
    }
    if (waiting != c->waiting) {
        c->waiting = waiting;
        lv_obj_set_hidden(c->wait, !waiting);
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
    bool answered = m->call == UI_CALL_ACTIVE || held;
    if (answered) {
        ui_fmt_duration(t, sizeof(t), m->call_seconds);
        lv_label_set_text(c->timer, t);
    } else {
        lv_label_set_text(c->timer, m->call == UI_CALL_RINGING_OUT ? "Ringing..." : "Calling...");
    }
    lv_obj_set_style_text_color(c->timer, held ? UI_COL_WARN : UI_COL_TEXT, 0);
    status_refresh(c, m);
    side_refresh(c, m);

    set_checked(c->mute, m->muted);
    set_checked(c->speaker, m->speaker);
    set_checked(c->hold, held);
    bool xfer_busy = m->xfer == UI_XFER_TRYING || m->xfer == UI_XFER_ACCEPTED ||
                     m->xfer == UI_XFER_PROGRESS;
    set_enabled(c->xfer, answered && !xfer_busy);
    set_enabled(c->complete, answered);
    lv_label_set_text(lv_obj_get_child(c->mute, 1), m->muted ? "Muted" : "Mute");
    lv_label_set_text(lv_obj_get_child(c->hold, 0), held ? LV_SYMBOL_PLAY : LV_SYMBOL_PAUSE);
    lv_label_set_text(lv_obj_get_child(c->hold, 1), held ? "Resume" : "Hold");
    lv_label_set_text_fmt(c->vol_val, "%u", (unsigned)m->volume);

    /* held background call (or the original call during a consultation) */
    if (two) {
        char tag[40];
        char d[16];
        ui_fmt_duration(d, sizeof(d), m->second_seconds);
        snprintf(tag, sizeof(tag), "%s  %s", consult ? "Original call, on hold" : "On hold", d);
        lv_label_set_text(c->strip_tag, tag);
        const char *name = m->second_name[0] ? m->second_name : m->second_number;
        ui_fit_set(c->strip_who, name[0] ? name : "Unknown", &lv_font_montserrat_24,
                   !m->second_name[0]);
        lv_obj_set_hidden(c->swap, consult);
    }
    if (waiting) {
        const char *name = m->second_name[0] ? m->second_name : m->second_number;
        ui_fit_set(c->wait_who, name[0] ? name : "Unknown", &lv_font_montserrat_24,
                   !m->second_name[0]);
        ui_fit_set(c->wait_sub, m->second_name[0] ? m->second_number : "", &lv_font_montserrat_14,
                   true);
    }
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
        if (c->consult) {
            ui_act_transfer_attended_cancel();
        } else {
            ui_act_hangup();
        }
    }
}

const ui_screen_ops_t ui_ops_call = {call_create, call_update, call_key, NULL, NULL};
