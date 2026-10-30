/* Incoming call: caller + big Answer / Decline. A find-the-handset call says so in the
 * title (amber). */

#include "ui_priv.h"

typedef struct {
    lv_obj_t *title;
    lv_obj_t *who;
    lv_obj_t *sub;
} incoming_t;

static incoming_t *s_i;

static void on_delete(lv_event_t *e)
{
    incoming_t *i = lv_event_get_user_data(e);
    if (s_i == i) {
        s_i = NULL;
    }
    lv_free(i);
}

static void on_answer(lv_event_t *e)
{
    (void)e;
    ui_act_answer();
}

static void on_decline(lv_event_t *e)
{
    (void)e;
    ui_act_hangup();
}

static lv_obj_t *incoming_create(const char *arg)
{
    (void)arg;
    incoming_t *i = lv_zalloc(sizeof(*i));
    if (!i) {
        return NULL;
    }
    lv_obj_t *scr = ui_screen_create();
    lv_obj_add_event_cb(scr, on_delete, LV_EVENT_DELETE, i);
    ui_statusbar_create(scr);

    i->title = ui_label_create(scr, &lv_font_montserrat_14, UI_COL_OK, "INCOMING CALL");
    lv_obj_set_style_text_letter_space(i->title, 2, 0);
    lv_obj_align(i->title, LV_ALIGN_TOP_MID, 0, 32);

    ui_avatar_create(scr, LV_SYMBOL_BELL, UI_COL_OK, 54);
    i->who = ui_fit_create(scr, 6, 144, 228, 42, UI_COL_TEXT);
    i->sub = ui_fit_create(scr, 6, 186, 228, 20, UI_COL_DIM);

    lv_obj_t *hint = ui_label_create(scr, &lv_font_montserrat_14, UI_COL_DIM,
                                     "# answer     * decline");
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 214);

    lv_obj_t *ans = ui_icon_btn_create(scr, UI_BTN_GREEN, LV_SYMBOL_CALL, &lv_font_montserrat_32,
                                       0, "Answer", &lv_font_montserrat_14, 6, 236, 114, 78);
    lv_obj_add_event_cb(ans, on_answer, LV_EVENT_CLICKED, NULL);
    lv_obj_t *dec = ui_icon_btn_create(scr, UI_BTN_RED, LV_SYMBOL_CALL, &lv_font_montserrat_32,
                                       2250, "Decline", &lv_font_montserrat_14, 126, 236, 108,
                                       78);
    lv_obj_add_event_cb(dec, on_decline, LV_EVENT_CLICKED, NULL);

    s_i = i;
    return scr;
}

static void incoming_update(const ui_model_t *m)
{
    incoming_t *i = s_i;
    if (!i) {
        return;
    }
    const char *main_txt, *sub_txt;
    ui_remote_texts(m, &main_txt, &sub_txt);
    ui_fit_set(i->who, main_txt, &lv_font_montserrat_32, !m->remote_name[0]);
    ui_fit_set(i->sub, sub_txt, &lv_font_montserrat_14, true);
    if (ui_label_set_text_if_changed(i->title, m->find_ringing ? "FIND HANDSET" : "INCOMING CALL")) {
        lv_obj_set_style_text_color(i->title, m->find_ringing ? UI_COL_WARN : UI_COL_OK, 0);
    }
}

static void incoming_key(char key, ui_key_action_t action)
{
    if (action == UI_KEY_REPEAT) {
        return;
    }
    if (key == '#') {
        ui_act_answer();
    } else if (key == '*') {
        ui_act_hangup(); /* decline */
    }
}

const ui_screen_ops_t ui_ops_incoming = {incoming_create, incoming_update, incoming_key, NULL, NULL};
