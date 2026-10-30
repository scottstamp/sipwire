/* Call history: newest first, one 50 px row per call (direction/result icon, name or number,
 * what happened, time, talk time). Tap = call back. "Clear" asks first. Opening the screen
 * marks the missed calls seen (after the list was fetched; they keep showing as new while the
 * screen is open).
 *
 * The list follows the history live: when the model's history_rev changes it is refetched in
 * place (no new widgets: the virtual list rebinds its visible rows). Calls added in front push
 * the list down by that many rows when it is scrolled, so the rows in view stay put; at the
 * top the new calls scroll into view. A new missed call shows as new, but is not marked seen
 * (nobody may be looking): the home badge keeps counting it. */

#include "ui_priv.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#define LIST_Y  (UI_BAR_H + UI_HEADER_H)
#define LIST_H  (UI_H - LIST_Y)
#define ROW_H   50
#define LIST_X  6
#define LIST_W  228

typedef struct {
    lv_obj_t *list;
    lv_obj_t *empty;
    lv_obj_t *clear;
    lv_obj_t *confirm;  /* modal overlay, NULL when closed */
    ui_call_t *items;
    int32_t n;
    bool fresh[UI_HISTORY_MAX];     /* row shown as a new missed call */
    uint32_t rev;                   /* model history_rev the list was fetched at */
    int64_t now;                    /* model now the times were formatted at */
} hist_t;

static hist_t *s_h;

static void on_delete(lv_event_t *e)
{
    hist_t *h = lv_event_get_user_data(e);
    if (s_h == h) {
        s_h = NULL;
    }
    lv_free(h->items);
    lv_free(h);
}

/* The five children of a row are created in this order and found by index. */
enum { C_ICON, C_NAME, C_SUB, C_WHEN };

static void make_row(lv_obj_t *row, void *user)
{
    (void)user;
    ui_label_create(row, &lv_font_montserrat_24, UI_COL_OK, "");
    lv_obj_set_pos(lv_obj_get_child(row, C_ICON), 8, 10);
    lv_obj_t *name = ui_fit_create(row, 36, 2, 186, 28, UI_COL_TEXT);
    lv_obj_set_style_text_align(lv_obj_get_child(name, 0), LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_t *sub = ui_label_create(row, &lv_font_montserrat_14, UI_COL_DIM, "");
    lv_obj_set_pos(sub, 36, 29);
    lv_obj_set_width(sub, 126);
    lv_obj_set_height(sub, 18);         /* one line: DOTS needs a fixed height */
    lv_label_set_long_mode(sub, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_t *when = ui_label_create(row, &lv_font_montserrat_14, UI_COL_DIM, "");
    lv_obj_set_width(when, 62);
    lv_obj_set_pos(when, LIST_W - 62 - 6, 29);
    lv_obj_set_style_text_align(when, LV_TEXT_ALIGN_RIGHT, 0);
}

static const char *icon_for(const ui_call_t *c, lv_color_t *col)
{
    bool out = c->direction == UI_CALLDIR_OUT;
    switch (c->result) {
    case UI_CALLRES_ANSWERED:
        *col = out ? UI_COL_BLUE : UI_COL_OK;
        return out ? LV_SYMBOL_UP : LV_SYMBOL_DOWN;
    case UI_CALLRES_MISSED:
        *col = out ? UI_COL_WARN : UI_COL_BAD;
        return out ? LV_SYMBOL_UP : LV_SYMBOL_DOWN;
    case UI_CALLRES_REJECTED:
        *col = UI_COL_WARN;
        return LV_SYMBOL_CLOSE;
    case UI_CALLRES_DND:
    default:
        *col = UI_COL_DIM;
        return LV_SYMBOL_MUTE;
    }
}

static const char *status_for(const ui_call_t *c)
{
    switch (c->result) {
    case UI_CALLRES_MISSED:   return c->direction == UI_CALLDIR_OUT ? "No answer" : "Missed";
    case UI_CALLRES_REJECTED: return "Declined";
    case UI_CALLRES_DND:      return "Do not disturb";
    default:                  return "";
    }
}

static void bind_row(lv_obj_t *row, int32_t idx, void *user)
{
    hist_t *h = user;
    const ui_call_t *c = &h->items[idx];
    lv_color_t col;
    const char *icon = icon_for(c, &col);
    bool fresh = h->fresh[idx];

    lv_obj_t *ic = lv_obj_get_child(row, C_ICON);
    lv_label_set_text(ic, icon);
    lv_obj_set_style_text_color(ic, col, 0);

    bool named = c->name[0] != '\0';
    if (named) {
        ui_fit_set_fixed(lv_obj_get_child(row, C_NAME), c->name, &lv_font_montserrat_24);
    } else {
        ui_fit_set(lv_obj_get_child(row, C_NAME), c->number[0] ? c->number : "Unknown", &lv_font_montserrat_24,
                   true);   /* a long number keeps its end */
    }
    ui_fit_set_color(lv_obj_get_child(row, C_NAME), fresh ? UI_COL_BAD : UI_COL_TEXT);

    const char *st = status_for(c);
    char dur[16] = "";
    if (c->result == UI_CALLRES_ANSWERED && c->duration_s > 0) {
        ui_fmt_duration(dur, sizeof(dur), c->duration_s);
    }
    /* line 2: "<status> - <number>" / "<number>  <talk time>" / just the status or talk time */
    char sub[UI_NUMBER_LEN + 32];
    if (named && st[0]) {
        snprintf(sub, sizeof(sub), "%s - %s", st, c->number);
    } else if (named) {
        snprintf(sub, sizeof(sub), "%s%s%s", c->number, dur[0] ? "  " : "", dur);
    } else {
        snprintf(sub, sizeof(sub), "%s%s", st, dur);
    }
    lv_label_set_text(lv_obj_get_child(row, C_SUB), sub);

    char when[16] = "";
    if (c->start > 0) {
        time_t t = (time_t)c->start;
        time_t nowt = ui_model()->now > 0 ? (time_t)ui_model()->now : t;
        struct tm tm_t, tm_n;
        localtime_r(&t, &tm_t);
        localtime_r(&nowt, &tm_n);
        ui_fmt_when(when, sizeof(when), &tm_t, &tm_n, ui_model()->clock_24h);
    }
    lv_label_set_text(lv_obj_get_child(row, C_WHEN), when);

}

static void clicked(int32_t idx, void *user)
{
    hist_t *h = user;
    if (idx >= 0 && idx < h->n && !h->confirm) {
        ui_act_dial(h->items[idx].number);
    }
}

static const ui_vlist_ops_t OPS = {make_row, bind_row, clicked};

static void on_back(lv_event_t *e)
{
    (void)e;
    ui_goto(UI_SCR_HOME, NULL);
}

static void confirm_close(hist_t *h)
{
    if (h->confirm) {
        lv_obj_delete(h->confirm);
        h->confirm = NULL;
    }
}

static void on_cancel(lv_event_t *e)
{
    confirm_close(lv_event_get_user_data(e));
}

/* (Re)fetch the list. Keeps the rows in view when calls were added in front of them. */
static void reload(hist_t *h)
{
    ui_call_t old_first;
    int32_t old_n = h->n;
    if (old_n > 0) {
        old_first = h->items[0];
    }
    int32_t sy = lv_obj_get_scroll_y(h->list);
    h->n = (int32_t)ui_act_history_list(h->items, UI_HISTORY_MAX);
    int32_t shift = old_n > 0 ? ui_hist_shift(&old_first, h->items, h->n) : -1;
    ui_hist_carry_fresh(h->fresh, old_n, shift, h->items, h->n);
    ui_vlist_set_count(h->list, h->n);
    if (shift > 0 && sy > 0) {
        lv_obj_scroll_to_y(h->list, sy + shift * ROW_H, LV_ANIM_OFF);
        ui_vlist_refresh(h->list);
    }
    lv_obj_set_hidden(h->empty, h->n != 0);
    lv_obj_set_hidden(h->clear, h->n == 0);
    if (h->n == 0) {
        confirm_close(h);
    }
}

static void on_confirm_clear(lv_event_t *e)
{
    hist_t *h = lv_event_get_user_data(e);
    ui_act_history_clear();
    reload(h);
    confirm_close(h);   /* last: deletes this button */
}

static void on_clear(lv_event_t *e)
{
    hist_t *h = lv_event_get_user_data(e);
    if (h->confirm) {
        return;
    }
    lv_obj_t *scr = lv_obj_get_parent(h->clear);
    scr = lv_obj_get_parent(scr);   /* header -> screen */
    h->confirm = ui_box_create(scr, 0, 0, UI_W, UI_H);
    lv_obj_set_clickable(h->confirm, true);       /* swallows taps outside the panel */
    lv_obj_set_style_bg_color(h->confirm, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(h->confirm, LV_OPA_70, 0);
    lv_obj_t *p = ui_box_create(h->confirm, 20, 100, 200, 128);
    lv_obj_set_style_bg_color(p, UI_COL_SURFACE, 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(p, 10, 0);
    lv_obj_set_style_border_width(p, 2, 0);
    lv_obj_set_style_border_color(p, UI_COL_SURFACE2, 0);
    lv_obj_t *t = ui_label_create(p, &lv_font_montserrat_24, UI_COL_TEXT, "Clear history?");
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 12);
    lv_obj_t *t2 = ui_label_create(p, &lv_font_montserrat_14, UI_COL_DIM, "All calls will be removed.");
    lv_obj_align(t2, LV_ALIGN_TOP_MID, 0, 46);
    lv_obj_t *no = ui_btn_create(p, UI_BTN_NEUTRAL, "No", 8, 72, 88, 46);
    lv_obj_add_event_cb(no, on_cancel, LV_EVENT_CLICKED, h);
    lv_obj_t *yes = ui_btn_create(p, UI_BTN_RED, "Clear", 104, 72, 88, 46);
    lv_obj_add_event_cb(yes, on_confirm_clear, LV_EVENT_CLICKED, h);
}

static lv_obj_t *history_create(const char *arg)
{
    (void)arg;
    hist_t *h = lv_zalloc(sizeof(*h));
    if (!h) {
        return NULL;
    }
    h->items = lv_malloc(sizeof(ui_call_t) * UI_HISTORY_MAX);
    if (!h->items) {
        lv_free(h);
        return NULL;
    }
    lv_obj_t *scr = ui_screen_create();
    lv_obj_add_event_cb(scr, on_delete, LV_EVENT_DELETE, h);

    h->list = ui_vlist_create(scr, LIST_X, LIST_Y, LIST_W, LIST_H, ROW_H, &OPS, h);
    h->empty = ui_label_create(scr, &lv_font_montserrat_24, UI_COL_DIM, "No calls yet");
    lv_obj_align(h->empty, LV_ALIGN_CENTER, 0, 10);
    lv_obj_set_hidden(h->empty, true);

    lv_obj_t *back = ui_header_create(scr, "History", UI_BAR_H, NULL);
    lv_obj_add_event_cb(back, on_back, LV_EVENT_CLICKED, NULL);
    h->clear = ui_btn_create(lv_obj_get_parent(back), UI_BTN_NEUTRAL, NULL, UI_W - 82, 2, 78, 44);
    lv_obj_t *cl = ui_label_create(h->clear, &lv_font_montserrat_14, UI_COL_TEXT, LV_SYMBOL_TRASH " Clear");
    lv_obj_center(cl);
    lv_obj_add_event_cb(h->clear, on_clear, LV_EVENT_CLICKED, h);
    ui_statusbar_create(scr);

    s_h = h;
    h->rev = ui_model()->history_rev;
    h->now = ui_model()->now;
    reload(h);
    bool unseen = false;
    for (int32_t i = 0; i < h->n; i++) {
        unseen = unseen || h->fresh[i];
    }
    if (unseen) {
        ui_act_history_mark_seen();     /* the model's history_rev follows: reload keeps the flags */
    }
    return scr;
}

static void history_update(const ui_model_t *m)
{
    hist_t *h = s_h;
    if (!h) {
        return;
    }
    if (m->history_rev != h->rev) {
        h->rev = m->history_rev;
        h->now = m->now;
        reload(h);
    } else if (m->now != h->now) {
        h->now = m->now;                /* "14:03" -> "Yesterday"...: rebind the visible rows */
        ui_vlist_refresh(h->list);
    }
}

static void history_key(char key, ui_key_action_t action)
{
    hist_t *h = s_h;
    if (!h || action != UI_KEY_PRESS) {
        return;
    }
    if (key == '*') {
        if (h->confirm) {
            confirm_close(h);
        } else {
            ui_goto(UI_SCR_HOME, NULL);
        }
    }
}

const ui_screen_ops_t ui_ops_history = {history_create, history_update, history_key, NULL, NULL};
