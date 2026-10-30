/* Call ended: shown while the model is in UI_CALL_ENDED. */

#include "ui_priv.h"

#include <stdio.h>

typedef struct {
    lv_obj_t *reason;
    lv_obj_t *who;
    lv_obj_t *dur;
    lv_obj_t *avatar;
} ended_t;

static ended_t *s_e;

static void on_delete(lv_event_t *e)
{
    ended_t *x = lv_event_get_user_data(e);
    if (s_e == x) {
        s_e = NULL;
    }
    lv_free(x);
}

static const char *reason_text(int code, char *buf, size_t cap)
{
    switch (code) {
    case 0:   return "Normal";
    case 302: return "Forwarded";
    case 486: return "Busy";
    case 487: return "Cancelled";
    case 404: return "Not found";
    case 480: return "Unavailable";
    case 408: return "No answer";
    case 603: return "Declined";
    default:
        snprintf(buf, cap, "Failed (%d)", code);
        return buf;
    }
}

static lv_obj_t *ended_create(const char *arg)
{
    (void)arg;
    ended_t *x = lv_zalloc(sizeof(*x));
    if (!x) {
        return NULL;
    }
    lv_obj_t *scr = ui_screen_create();
    lv_obj_add_event_cb(scr, on_delete, LV_EVENT_DELETE, x);
    ui_statusbar_create(scr);

    x->avatar = ui_avatar_create(scr, LV_SYMBOL_CLOSE, UI_COL_BAD, 50);

    lv_obj_t *title = ui_label_create(scr, &lv_font_montserrat_32, UI_COL_TEXT, "Call ended");
    lv_obj_set_width(title, UI_W);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 150);

    x->reason = ui_fit_create(scr, 6, 194, 228, 30, UI_COL_WARN);
    x->who = ui_fit_create(scr, 6, 232, 228, 30, UI_COL_TEXT);
    x->dur = ui_fit_create(scr, 6, 266, 228, 20, UI_COL_DIM);

    s_e = x;
    return scr;
}

static void ended_update(const ui_model_t *m)
{
    ended_t *x = s_e;
    if (!x) {
        return;
    }
    char buf[24];
    bool xferred = m->xfer == UI_XFER_DONE; /* call features: the call was transferred away */
    ui_fit_set(x->reason, xferred ? "Transferred" : reason_text(m->end_code, buf, sizeof(buf)),
               &lv_font_montserrat_24, false);
    lv_obj_t *sym = lv_obj_get_child(x->avatar, 0);
    lv_label_set_text(sym, xferred ? LV_SYMBOL_OK : LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_color(sym, xferred ? UI_COL_OK : UI_COL_BAD, 0);
    lv_obj_set_style_border_color(x->avatar, xferred ? UI_COL_OK : UI_COL_BAD, 0);
    ui_fit_set_color(x->reason, xferred ? UI_COL_OK : (m->end_code == 0 ? UI_COL_DIM : UI_COL_WARN));

    const char *main_txt, *sub_txt;
    ui_remote_texts(m, &main_txt, &sub_txt);
    ui_fit_set(x->who, main_txt, &lv_font_montserrat_24, !m->remote_name[0]);

    if (xferred) {
        char line[96];
        snprintf(line, sizeof(line), "to %s", ui_target_name(m, m->xfer_target));
        ui_fit_set(x->dur, line, &lv_font_montserrat_14, true);
    } else if (m->call_seconds > 0) {
        char t[16];
        char line[40];
        ui_fmt_duration(t, sizeof(t), m->call_seconds);
        snprintf(line, sizeof(line), "Duration %s", t);
        ui_fit_set(x->dur, line, &lv_font_montserrat_14, false);
    } else {
        ui_fit_set(x->dur, "", &lv_font_montserrat_14, false);
    }
}

const ui_screen_ops_t ui_ops_ended = {ended_create, ended_update, NULL, NULL, NULL};
