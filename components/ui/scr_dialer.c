/* Dialer: number display, contact matches, touch keypad, back / call / backspace.
 *
 * Under the number the best contact match (number substring, see ui_actions_t.contacts_search)
 * is shown with a "+N" for more; tapping it fills in its number. Holding 2-9 on the physical or
 * the on-screen keypad calls the speed dial entry of that key when the number is still empty
 * (physical: just that digit). A lone 2-9 that has an entry shows "Hold 5 to call Bob". */

#include "ui_priv.h"

#include <stdio.h>
#include <string.h>

#define NUM_MAX 40
#define MATCH_MAX 10

typedef struct {
    lv_obj_t *num;  /* fitted label */
    lv_obj_t *hint; /* "Enter number" placeholder */
    lv_obj_t *match;     /* tappable strip with the best contact match */
    lv_obj_t *m_name;
    lv_obj_t *m_num;
    lv_obj_t *m_more;
    ui_contact_t best;   /* what the strip would fill in */
    bool strip_tappable;
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

static void refresh_match(dialer_t *d)
{
    ui_contact_t found[MATCH_MAX];
    size_t n = 0;
    ui_contact_t sp;
    bool hold = d->len == 1 && speed_target(d->buf[0], &sp);
    if (d->len && !hold) {
        n = ui_act_contacts_search(d->buf, found, MATCH_MAX);
    }
    if (!hold && n == 0) {
        lv_obj_set_hidden(d->match, true);
        return;
    }
    lv_obj_set_hidden(d->match, false);
    if (hold) {
        lv_label_set_text_fmt(d->m_name, "Hold %c to call %s", d->buf[0], sp.name[0] ? sp.name : sp.number);
        lv_obj_set_style_text_color(d->m_name, UI_COL_WARN, 0);
        lv_label_set_text(d->m_num, "");
        lv_label_set_text(d->m_more, "");
        lv_obj_set_width(d->m_name, 216);
        d->strip_tappable = false;
        return;
    }
    d->best = found[0];
    d->strip_tappable = true;
    lv_obj_set_width(d->m_name, 100);
    lv_obj_set_style_text_color(d->m_name, UI_COL_TEXT, 0);
    /* the name gets what the number and the "+N" leave (14 pt digits are ~8 px wide) */
    int32_t nw = 196 - (int32_t)strlen(found[0].number) * 8 - (n > 1 ? 28 : 0);
    lv_obj_set_width(d->m_name, nw < 60 ? 60 : nw);
    lv_label_set_text(d->m_name, found[0].name[0] ? found[0].name : "Contact");
    lv_label_set_text(d->m_num, found[0].number);
    if (n > 1) {
        lv_label_set_text_fmt(d->m_more, "+%u%s", (unsigned)(n - 1), n == MATCH_MAX ? "+" : "");
    } else {
        lv_label_set_text(d->m_more, "");
    }
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

static void on_match(lv_event_t *e)
{
    dialer_t *d = lv_event_get_user_data(e);
    if (d->strip_tappable) {
        snprintf(d->buf, sizeof(d->buf), "%s", d->best.number);
        d->len = strlen(d->buf);
        refresh(d);
    }
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

    d->num = ui_fit_create(scr, 6, 24, 228, 38, UI_COL_TEXT);
    d->hint = ui_label_create(scr, &lv_font_montserrat_14, UI_COL_DIM, "Enter number");
    lv_obj_align(d->hint, LV_ALIGN_TOP_MID, 0, 36);

    /* best contact match under the number */
    d->match = ui_btn_create(scr, UI_BTN_NEUTRAL, NULL, 6, 62, 228, 30);
    lv_obj_set_flex_flow(d->match, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(d->match, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(d->match, 8, 0);
    lv_obj_set_style_pad_column(d->match, 6, 0);
    d->m_name = ui_label_create(d->match, &lv_font_montserrat_14, UI_COL_TEXT, "");
    lv_label_set_long_mode(d->m_name, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(d->m_name, 100);
    lv_obj_set_height(d->m_name, 18);   /* one line: DOTS needs a fixed height */
    d->m_num = ui_label_create(d->match, &lv_font_montserrat_14, UI_COL_DIM, "");
    d->m_more = ui_label_create(d->match, &lv_font_montserrat_14, UI_COL_BLUE, "");
    lv_obj_add_event_cb(d->match, on_match, LV_EVENT_CLICKED, d);
    lv_obj_set_hidden(d->match, true);

    lv_obj_t *kp = ui_keypad_create(scr, 6, 95, 228, 176, on_key, d);
    lv_obj_set_style_pad_row(kp, 4, LV_PART_MAIN);

    lv_obj_t *back = ui_btn_create(scr, UI_BTN_NEUTRAL, LV_SYMBOL_LEFT, 6, 275, 60, 42);
    lv_obj_add_event_cb(back, on_back, LV_EVENT_CLICKED, NULL);
    lv_obj_t *call = ui_btn_create(scr, UI_BTN_GREEN, LV_SYMBOL_CALL, 72, 275, 96, 42);
    lv_obj_add_event_cb(call, on_call, LV_EVENT_CLICKED, d);
    lv_obj_t *bk = ui_btn_create(scr, UI_BTN_NEUTRAL, LV_SYMBOL_BACKSPACE, 174, 275, 60, 42);
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
