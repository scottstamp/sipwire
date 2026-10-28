/* Dialer: number display, touch keypad, back / call / backspace. */

#include "ui_priv.h"

#include <string.h>

#define NUM_MAX 40

typedef struct {
    lv_obj_t *num;  /* fitted label */
    lv_obj_t *hint; /* "Enter number" placeholder */
    char buf[NUM_MAX + 1];
    size_t len;
} dialer_t;

static dialer_t *s_d;

static void on_delete(lv_event_t *e)
{
    dialer_t *d = lv_event_get_user_data(e);
    if (s_d == d) {
        s_d = NULL;
    }
    lv_free(d);
}

static void refresh(dialer_t *d)
{
    ui_fit_set(d->num, d->buf, &lv_font_montserrat_32, true);
    if (d->len) {
        lv_obj_set_hidden(d->hint, true);
    } else {
        lv_obj_set_hidden(d->hint, false);
    }
}

static void append(dialer_t *d, char c)
{
    if (d->len < NUM_MAX) {
        d->buf[d->len++] = c;
        d->buf[d->len] = '\0';
        refresh(d);
    }
}

static void backspace(dialer_t *d)
{
    if (d->len) {
        d->buf[--d->len] = '\0';
        refresh(d);
    }
}

static void clear_all(dialer_t *d)
{
    d->len = 0;
    d->buf[0] = '\0';
    refresh(d);
}

static void do_call(dialer_t *d)
{
    if (d->len) {
        ui_act_dial(d->buf);
    }
}

static void on_key(char key, bool long_press, void *user)
{
    append(user, long_press ? '+' : key);
}

static void on_back(lv_event_t *e)
{
    (void)e;
    ui_goto(UI_SCR_HOME, NULL);
}

static void on_call(lv_event_t *e)
{
    do_call(lv_event_get_user_data(e));
}

static void on_bksp(lv_event_t *e)
{
    dialer_t *d = lv_event_get_user_data(e);
    if (lv_event_get_code(e) == LV_EVENT_LONG_PRESSED) {
        clear_all(d); /* hold = clear the whole number */
    } else {
        backspace(d);
    }
}

static lv_obj_t *dialer_create(const char *arg)
{
    dialer_t *d = lv_zalloc(sizeof(*d));
    if (!d) {
        return NULL;
    }
    lv_obj_t *scr = ui_screen_create();
    lv_obj_add_event_cb(scr, on_delete, LV_EVENT_DELETE, d);
    ui_statusbar_create(scr);

    d->num = ui_fit_create(scr, 6, 26, 228, 42, UI_COL_TEXT);
    d->hint = ui_label_create(scr, &lv_font_montserrat_14, UI_COL_DIM, "Enter number");
    lv_obj_align(d->hint, LV_ALIGN_TOP_MID, 0, 40);

    ui_keypad_create(scr, 6, 72, 228, 194, on_key, d);

    lv_obj_t *back = ui_btn_create(scr, UI_BTN_NEUTRAL, LV_SYMBOL_LEFT, 6, 272, 60, 44);
    lv_obj_add_event_cb(back, on_back, LV_EVENT_CLICKED, NULL);
    lv_obj_t *call = ui_btn_create(scr, UI_BTN_GREEN, LV_SYMBOL_CALL, 72, 272, 96, 44);
    lv_obj_add_event_cb(call, on_call, LV_EVENT_CLICKED, d);
    lv_obj_t *bk = ui_btn_create(scr, UI_BTN_NEUTRAL, LV_SYMBOL_BACKSPACE, 174, 272, 60, 44);
    lv_obj_add_event_cb(bk, on_bksp, LV_EVENT_CLICKED, d);
    lv_obj_add_event_cb(bk, on_bksp, LV_EVENT_LONG_PRESSED, d);

    s_d = d;
    if (arg) {
        for (; *arg; arg++) {
            if (d->len < NUM_MAX) {
                d->buf[d->len++] = *arg;
            }
        }
        d->buf[d->len] = '\0';
    }
    refresh(d);
    return scr;
}

static void dialer_update(const ui_model_t *m)
{
    (void)m; /* nothing model dependent; the router switches away on calls */
}

static void dialer_key(char key, ui_key_action_t action)
{
    dialer_t *d = s_d;
    if (!d) {
        return;
    }
    switch (action) {
    case UI_KEY_PRESS:
        if (ui_is_dial_key(key)) {
            append(d, key);
        }
        break;
    case UI_KEY_LONG:
        if (key == '0') {
            append(d, '+');
        } else if (key == '#') {
            do_call(d);
        } else if (key == '*') {
            if (d->len) {
                backspace(d);
            } else {
                ui_goto(UI_SCR_HOME, NULL); /* nothing left to delete: leave */
            }
        }
        break;
    case UI_KEY_REPEAT:
        if (key == '*') {
            backspace(d);
        }
        break;
    }
}

const ui_screen_ops_t ui_ops_dialer = {dialer_create, dialer_update, dialer_key};
