/* Outgoing call (DIALING / RINGING_OUT): who we call + hang up. */

#include "ui_priv.h"

typedef struct {
    lv_obj_t *who;
    lv_obj_t *sub;
    lv_obj_t *status;
} outgoing_t;

static outgoing_t *s_o;

static void on_delete(lv_event_t *e)
{
    outgoing_t *o = lv_event_get_user_data(e);
    if (s_o == o) {
        s_o = NULL;
    }
    lv_free(o);
}

static void on_hangup(lv_event_t *e)
{
    (void)e;
    ui_act_hangup();
}

static lv_obj_t *outgoing_create(const char *arg)
{
    (void)arg;
    outgoing_t *o = lv_zalloc(sizeof(*o));
    if (!o) {
        return NULL;
    }
    lv_obj_t *scr = ui_screen_create();
    lv_obj_add_event_cb(scr, on_delete, LV_EVENT_DELETE, o);
    ui_statusbar_create(scr);

    ui_avatar_create(scr, LV_SYMBOL_CALL, UI_COL_BLUE, 40);
    o->who = ui_fit_create(scr, 6, 134, 228, 42, UI_COL_TEXT);
    o->sub = ui_fit_create(scr, 6, 176, 228, 20, UI_COL_DIM);
    o->status = ui_fit_create(scr, 6, 204, 228, 30, UI_COL_WARN);

    lv_obj_t *hang = ui_hangup_btn_create(scr, "Hang up", 6, 254, 228, 60);
    lv_obj_add_event_cb(hang, on_hangup, LV_EVENT_CLICKED, NULL);

    s_o = o;
    return scr;
}

static void outgoing_update(const ui_model_t *m)
{
    outgoing_t *o = s_o;
    if (!o) {
        return;
    }
    const char *main_txt, *sub_txt;
    ui_remote_texts(m, &main_txt, &sub_txt);
    ui_fit_set(o->who, main_txt, &lv_font_montserrat_32, !m->remote_name[0]);
    ui_fit_set(o->sub, sub_txt, &lv_font_montserrat_14, true);
    ui_fit_set(o->status, m->call == UI_CALL_RINGING_OUT ? "Ringing..." : "Calling...",
               &lv_font_montserrat_24, false);
}

static void outgoing_key(char key, ui_key_action_t action)
{
    if (action == UI_KEY_LONG && key == '*') {
        ui_act_hangup();
    }
}

const ui_screen_ops_t ui_ops_outgoing = {outgoing_create, outgoing_update, outgoing_key};
