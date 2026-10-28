/* Idle screen: clock, account, registration state, Dial / Settings. */

#include "ui_priv.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    lv_obj_t *clock;
    lv_obj_t *ampm;             /* "AM"/"PM" next to the big clock; hidden in 24 h mode */
    lv_obj_t *name;
    lv_obj_t *id;
    lv_obj_t *state;
    lv_obj_t *sub;
} home_t;

static home_t *s_h;

static void on_delete(lv_event_t *e)
{
    home_t *h = lv_event_get_user_data(e);
    if (s_h == h) {
        s_h = NULL;
    }
    lv_free(h);
}

static void on_dial(lv_event_t *e)
{
    (void)e;
    ui_goto(UI_SCR_DIALER, NULL);
}

static void on_settings(lv_event_t *e)
{
    (void)e;
    ui_goto(UI_SCR_SETTINGS, NULL);
}

static lv_obj_t *home_create(const char *arg)
{
    (void)arg;
    home_t *h = lv_zalloc(sizeof(*h));
    if (!h) {
        return NULL;
    }
    lv_obj_t *scr = ui_screen_create();
    lv_obj_add_event_cb(scr, on_delete, LV_EVENT_DELETE, h);
    ui_statusbar_create(scr);

    /* "3:07" in 40 pt with a 24 pt "PM" beside it, the pair centred as one. */
    lv_obj_t *crow = ui_box_create(scr, 0, 44, UI_W, 48);
    lv_obj_set_flex_flow(crow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(crow, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(crow, 6, 0);
    h->clock = ui_label_create(crow, &lv_font_montserrat_40, UI_COL_TEXT, "");
    h->ampm = ui_label_create(crow, &lv_font_montserrat_24, UI_COL_DIM, "");
    lv_obj_set_style_translate_y(h->ampm, -3, 0);
    lv_obj_set_hidden(h->ampm, true);

    h->name = ui_fit_create(scr, 6, 108, 228, 34, UI_COL_TEXT);
    h->id = ui_fit_create(scr, 6, 142, 228, 20, UI_COL_DIM);

    h->state = lv_label_create(scr);
    lv_obj_set_style_text_font(h->state, &lv_font_montserrat_24, 0);
    lv_obj_set_width(h->state, 216);
    lv_obj_set_style_text_align(h->state, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(h->state, LV_ALIGN_TOP_MID, 0, 176);

    h->sub = ui_label_create(scr, &lv_font_montserrat_14, UI_COL_BAD, "");
    lv_obj_set_width(h->sub, 216);
    lv_obj_set_style_text_align(h->sub, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(h->sub, LV_ALIGN_TOP_MID, 0, 212);

    lv_obj_t *dial = ui_icon_btn_create(scr, UI_BTN_GREEN, LV_SYMBOL_CALL, &lv_font_montserrat_24,
                                        0, "Dial", &lv_font_montserrat_14, 6, 250, 112, 64);
    lv_obj_add_event_cb(dial, on_dial, LV_EVENT_CLICKED, NULL);
    lv_obj_t *set = ui_icon_btn_create(scr, UI_BTN_NEUTRAL, LV_SYMBOL_SETTINGS,
                                       &lv_font_montserrat_24, 0, "Settings",
                                       &lv_font_montserrat_14, 122, 250, 112, 64);
    lv_obj_add_event_cb(set, on_settings, LV_EVENT_CLICKED, NULL);

    s_h = h;
    return scr;
}

/* "Display Name <user@server>" -> name + id; a bare "user" is the name. */
static void split_account(const char *acc, char *name, size_t ncap, char *id, size_t icap)
{
    name[0] = id[0] = '\0';
    const char *lt = strchr(acc, '<');
    const char *gt = lt ? strchr(lt, '>') : NULL;
    if (lt && gt) {
        size_t n = (size_t)(lt - acc);
        while (n > 0 && acc[n - 1] == ' ') {
            n--;
        }
        snprintf(name, ncap, "%.*s", (int)n, acc);
        snprintf(id, icap, "%.*s", (int)(gt - lt - 1), lt + 1);
        if (!name[0]) {
            snprintf(name, ncap, "%s", id);
            id[0] = '\0';
        }
    } else {
        snprintf(name, ncap, "%s", acc);
    }
}

static void home_update(const ui_model_t *m)
{
    home_t *h = s_h;
    if (!h) {
        return;
    }

    if (m->time_hhmm[0]) {
        /* "3:07 PM" -> "3:07" + "PM"; 24 h mode has no suffix */
        const char *sp = strchr(m->time_hhmm, ' ');
        char hm[UI_TIME_STR_LEN];
        snprintf(hm, sizeof(hm), "%.*s", sp ? (int)(sp - m->time_hhmm) : (int)sizeof(hm) - 1,
                 m->time_hhmm);
        lv_label_set_text(h->clock, hm);
        lv_obj_set_style_text_color(h->clock, UI_COL_TEXT, 0);
        lv_label_set_text(h->ampm, sp ? sp + 1 : "");
        lv_obj_set_hidden(h->ampm, sp == NULL);
    } else {
        lv_label_set_text(h->clock, "sipwire");
        lv_obj_set_style_text_color(h->clock, UI_COL_DIM, 0);
        lv_obj_set_hidden(h->ampm, true);
    }

    char name[64], id[64];
    split_account(m->account, name, sizeof(name), id, sizeof(id));
    ui_fit_set(h->name, name[0] ? name : "No account", &lv_font_montserrat_24, false);
    ui_fit_set_color(h->name, name[0] ? UI_COL_TEXT : UI_COL_DIM);
    ui_fit_set(h->id, id, &lv_font_montserrat_14, false);

    const char *icon;
    const char *txt;
    const char *sub = "";
    lv_color_t col;
    if (m->net == UI_NET_OFF) {
        icon = LV_SYMBOL_CLOSE;
        txt = "Offline";
        sub = "No Wi-Fi";
        col = UI_COL_BAD;
    } else {
        switch (m->reg) {
        case UI_REG_REGISTERED:
            icon = LV_SYMBOL_OK;
            txt = "Ready";
            col = UI_COL_OK;
            break;
        case UI_REG_REGISTERING:
            icon = LV_SYMBOL_REFRESH;
            txt = "Registering...";
            col = UI_COL_WARN;
            break;
        case UI_REG_FAILED:
            icon = LV_SYMBOL_WARNING;
            txt = "Registration failed";
            col = UI_COL_BAD;
            break;
        default:
            icon = LV_SYMBOL_CLOSE;
            txt = "Offline";
            col = UI_COL_DIM;
            break;
        }
        if (m->net == UI_NET_CONNECTING) {
            sub = "Connecting to Wi-Fi...";
        }
    }
    lv_label_set_text_fmt(h->state, "%s %s", icon, txt);
    lv_obj_set_style_text_color(h->state, col, 0);
    lv_label_set_text(h->sub, sub);
}

static void home_key(char key, ui_key_action_t action)
{
    if (action == UI_KEY_PRESS && ui_is_dial_key(key)) {
        char first[2] = {key, '\0'};
        ui_goto(UI_SCR_DIALER, first);
    } else if (action == UI_KEY_LONG && key == '0') {
        ui_goto(UI_SCR_DIALER, "+");
    }
}

const ui_screen_ops_t ui_ops_home = {home_create, home_update, home_key, NULL, NULL};
