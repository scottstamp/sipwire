/* Dialer: number display, contact matches, touch keypad, back / call / backspace.
 *
 * Under the number up to three contact matches (number substring, see
 * ui_actions_t.contacts_search), best first, one 24 px row each; when there are more, the last
 * row ends in "+N". Tap a match = fill in its number, hold it = call it. The match area has a
 * fixed height, so the keypad never moves while typing. Holding 2-9 on the physical or the
 * on-screen keypad calls the speed dial entry of that key when the number is still empty
 * (physical: just that digit). A lone 2-9 that has an entry shows "Hold 5 to call Bob" in the
 * first row.
 *
 * Layout (240x320): status bar 0-21, number 22-58, matches 60-136 (3 x 24 + 2 x 2),
 * keypad 139-273, Back | Call | Backspace 276-317. */

#include "ui_priv.h"

#include <stdio.h>
#include <string.h>

#define NUM_MAX 40
#define MATCH_MAX 10        /* fetched; "+N" counts up to this ("+7+" beyond) */
#define ROWS 3              /* shown */
#define ROW_Y 60
#define ROW_H 24
#define ROW_GAP 2
#define ROW_W 228
#define KP_Y (ROW_Y + ROWS * ROW_H + (ROWS - 1) * ROW_GAP + 3)
#define BTN_Y 276
#define BTN_H 41

typedef struct {
    lv_obj_t *btn;
    lv_obj_t *name;
    lv_obj_t *num;
    lv_obj_t *more;         /* "+N", last row only */
} match_row_t;

typedef struct {
    lv_obj_t *num;  /* fitted label */
    lv_obj_t *hint; /* "Enter number" placeholder */
    match_row_t row[ROWS];
    ui_contact_t match[ROWS];   /* what each row fills in / calls */
    int n_match;                /* rows bound to match[] (0 while the speed dial hint shows) */
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

/* Speed dial key held for this digit and still free to be one (nothing else typed)? */
static bool speed_target(char key, ui_contact_t *out)
{
    return key >= '0' + UI_SPEED_FIRST && key <= '0' + UI_SPEED_LAST && ui_act_speed_get(key - '0', out);
}

/* One match row: name left (dots when long), number dim at the right, "+N" after it. */
static void row_bind(match_row_t *r, const char *name, lv_color_t name_col, const char *num, const char *more)
{
    lv_label_set_text(r->num, num);
    lv_label_set_text(r->more, more);
    lv_obj_set_hidden(r->more, more[0] == '\0');
    lv_obj_update_layout(r->num);
    lv_obj_update_layout(r->more);
    /* the name gets what the number and the "+N" leave (padding 8 + 8, column gap 6) */
    int32_t w = ROW_W - 16 - (num[0] ? lv_obj_get_width(r->num) + 6 : 0) - (more[0] ? lv_obj_get_width(r->more) + 6 : 0);
    lv_obj_set_width(r->name, w < 48 ? 48 : w);
    lv_label_set_text(r->name, name);
    lv_obj_set_style_text_color(r->name, name_col, 0);
    lv_obj_set_hidden(r->btn, false);
}

static void refresh_match(dialer_t *d)
{
    ui_contact_t found[MATCH_MAX];
    size_t n = 0;
    ui_contact_t sp;
    bool hold = d->len == 1 && speed_target(d->buf[0], &sp);
    if (d->len && !hold) {
        n = ui_act_contacts_search(d->buf, found, MATCH_MAX);
    }
    for (int i = 0; i < ROWS; i++) {
        lv_obj_set_hidden(d->row[i].btn, true);
    }
    d->n_match = 0;
    if (hold) {
        char txt[UI_NAME_LEN + 24];
        snprintf(txt, sizeof(txt), "Hold %c to call %s", d->buf[0], sp.name[0] ? sp.name : sp.number);
        row_bind(&d->row[0], txt, UI_COL_WARN, "", "");
        return;
    }
    int shown = n < ROWS ? (int)n : ROWS;
    for (int i = 0; i < shown; i++) {
        char more[16] = "";
        if (i == shown - 1 && n > (size_t)shown) {
            snprintf(more, sizeof(more), "+%u%s", (unsigned)(n - (size_t)shown), n == MATCH_MAX ? "+" : "");
        }
        d->match[i] = found[i];
        row_bind(&d->row[i], found[i].name[0] ? found[i].name : "Contact", UI_COL_TEXT, found[i].number, more);
    }
    d->n_match = shown;
}

static void refresh(dialer_t *d)
{
    ui_fit_set(d->num, d->buf, &lv_font_montserrat_32, true);
    if (d->len) {
        lv_obj_set_hidden(d->hint, true);
    } else {
        lv_obj_set_hidden(d->hint, false);
    }
    refresh_match(d);
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
    dialer_t *d = user;
    ui_contact_t sp;
    if (long_press && key == '0') {
        append(d, '+');
    } else if (long_press && d->len == 0 && speed_target(key, &sp)) {
        ui_act_dial(sp.number);
    } else {
        append(d, key);
    }
}

/* Tap = fill in the number, hold = call it. */
static void on_match(lv_event_t *e)
{
    dialer_t *d = s_d;
    intptr_t i = (intptr_t)lv_event_get_user_data(e);
    if (!d || i < 0 || i >= d->n_match) {
        return;
    }
    if (lv_event_get_code(e) == LV_EVENT_LONG_PRESSED) {
        ui_act_dial(d->match[i].number);
        return;
    }
    snprintf(d->buf, sizeof(d->buf), "%s", d->match[i].number);
    d->len = strlen(d->buf);
    refresh(d);
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

    d->num = ui_fit_create(scr, 6, 22, 228, 36, UI_COL_TEXT);
    d->hint = ui_label_create(scr, &lv_font_montserrat_14, UI_COL_DIM, "Enter number");
    lv_obj_align(d->hint, LV_ALIGN_TOP_MID, 0, 32);

    /* contact matches under the number */
    for (int i = 0; i < ROWS; i++) {
        match_row_t *r = &d->row[i];
        r->btn = ui_btn_create(scr, UI_BTN_NEUTRAL, NULL, 6, ROW_Y + i * (ROW_H + ROW_GAP), ROW_W, ROW_H);
        lv_obj_set_style_radius(r->btn, 6, 0);
        lv_obj_set_flex_flow(r->btn, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(r->btn, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_hor(r->btn, 8, 0);
        lv_obj_set_style_pad_ver(r->btn, 0, 0);
        lv_obj_set_style_pad_column(r->btn, 6, 0);
        r->name = ui_label_create(r->btn, &lv_font_montserrat_14, UI_COL_TEXT, "");
        lv_label_set_long_mode(r->name, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_height(r->name, 18);     /* one line: DOTS needs a fixed height */
        r->num = ui_label_create(r->btn, &lv_font_montserrat_14, UI_COL_DIM, "");
        r->more = ui_label_create(r->btn, &lv_font_montserrat_14, UI_COL_BLUE, "");
        lv_obj_add_event_cb(r->btn, on_match, LV_EVENT_SHORT_CLICKED, (void *)(intptr_t)i);
        lv_obj_add_event_cb(r->btn, on_match, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)i);
        lv_obj_set_hidden(r->btn, true);
    }

    lv_obj_t *kp = ui_keypad_create(scr, 6, KP_Y, 228, BTN_Y - 3 - KP_Y, on_key, d);
    lv_obj_set_style_pad_row(kp, 3, LV_PART_MAIN);

    lv_obj_t *back = ui_btn_create(scr, UI_BTN_NEUTRAL, LV_SYMBOL_LEFT, 6, BTN_Y, 60, BTN_H);
    lv_obj_add_event_cb(back, on_back, LV_EVENT_CLICKED, NULL);
    lv_obj_t *call = ui_btn_create(scr, UI_BTN_GREEN, LV_SYMBOL_CALL, 72, BTN_Y, 96, BTN_H);
    lv_obj_add_event_cb(call, on_call, LV_EVENT_CLICKED, d);
    lv_obj_t *bk = ui_btn_create(scr, UI_BTN_NEUTRAL, LV_SYMBOL_BACKSPACE, 174, BTN_Y, 60, BTN_H);
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
    case UI_KEY_LONG: {
        /* The PRESS for this key already appended it (possibly on the home
         * screen, which opened us with it): take it back first. */
        bool undone = false;
        if (ui_is_dial_key(key) && d->len && d->buf[d->len - 1] == key) {
            backspace(d);
            undone = true;
        }
        ui_contact_t sp;
        if (key == '0') {
            append(d, '+');
        } else if (d->len == 0 && speed_target(key, &sp)) {
            ui_act_dial(sp.number);     /* held 2-9 with nothing else typed: speed dial */
        } else if (undone && ui_is_dial_key(key) && key != '#' && key != '*') {
            append(d, key);             /* no speed dial entry: keep the digit */
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
    }
    case UI_KEY_REPEAT:
        if (key == '*') {
            backspace(d);
        }
        break;
    }
}

const ui_screen_ops_t ui_ops_dialer = {dialer_create, dialer_update, dialer_key, NULL, NULL};
