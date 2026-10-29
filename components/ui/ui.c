/* UI router: owns the current screen, derives which screen to show from the
 * model's call state, and forwards model updates / physical keys to it.
 *
 * Screens are created lazily on every transition and deleted shortly after
 * they are left (never while one of their own event callbacks may still be
 * running), so nothing accumulates across calls.
 *
 * Physical key semantics (components/keypad): UI_KEY_PRESS on key-down (so
 * DTMF is instant), UI_KEY_LONG once after ~800 ms if still held, then
 * UI_KEY_REPEAT. A LONG is therefore always preceded by a PRESS of the same
 * key; screens that give LONG its own meaning must undo the PRESS effect. */

#include "ui_priv.h"

#include <string.h>

#define ANIM_MS 120

static const ui_screen_ops_t *const OPS[UI_SCR_COUNT] = {
    [UI_SCR_HOME] = &ui_ops_home,
    [UI_SCR_DIALER] = &ui_ops_dialer,
    [UI_SCR_OUTGOING] = &ui_ops_outgoing,
    [UI_SCR_INCOMING] = &ui_ops_incoming,
    [UI_SCR_CALL] = &ui_ops_call,
    [UI_SCR_ENDED] = &ui_ops_ended,
    [UI_SCR_SETTINGS] = &ui_ops_settings,
    [UI_SCR_CALIBRATE] = &ui_ops_calibrate,
    [UI_SCR_HISTORY] = &ui_ops_history,
    [UI_SCR_CONTACTS] = &ui_ops_contacts,
};

static const ui_actions_t *s_actions;
static ui_model_t s_model;
static ui_screen_t s_kind;
static lv_obj_t *s_scr; /* newest screen (may still be animating in) */
static bool s_inited;

const ui_model_t *ui_model(void)
{
    return &s_model;
}

/* ---- screen switching ---------------------------------------------------- */

static void show(ui_screen_t kind, const char *arg, lv_screen_load_anim_t anim)
{
    lv_obj_t *old = s_scr;
    lv_obj_t *scr = OPS[kind]->create(arg);
    if (!scr) {
        return;
    }
    if (old && OPS[s_kind]->leave) {
        OPS[s_kind]->leave(); /* e.g. calibration must end now, not when `old` is deleted */
    }
    s_scr = scr;
    s_kind = kind;
    ui_statusbar_update(&s_model);
    OPS[kind]->update(&s_model);

    uint32_t t = (anim == LV_SCREEN_LOAD_ANIM_NONE) ? 0 : ANIM_MS;
    lv_screen_load_anim(scr, anim, t, 0, false);
    if (old && old != scr) {
        /* Not before the transition is over, and never from inside a callback
         * of the old screen itself. */
        lv_obj_delete_delayed(old, t + 100);
    }
}

void ui_goto(ui_screen_t screen, const char *arg)
{
    if (!s_inited || screen >= UI_SCR_COUNT || screen == s_kind) {
        return;
    }
    bool back = screen == UI_SCR_HOME || (s_kind == UI_SCR_CALIBRATE && screen == UI_SCR_SETTINGS);
    show(screen, arg, back ? LV_SCREEN_LOAD_ANIM_MOVE_RIGHT : LV_SCREEN_LOAD_ANIM_MOVE_LEFT);
}

static bool is_call_screen(ui_screen_t s)
{
    return s == UI_SCR_OUTGOING || s == UI_SCR_INCOMING || s == UI_SCR_CALL || s == UI_SCR_ENDED;
}

void ui_init(lv_display_t *disp, const ui_actions_t *actions)
{
    s_actions = actions;
    memset(&s_model, 0, sizeof(s_model));
    s_model.battery_pct = -1;

    /* Screens are created lazily later; make sure they land on `disp`. */
    lv_display_set_default(disp);
    ui_theme_init();

    lv_obj_t *boot = lv_display_get_screen_active(disp);
    s_inited = true;
    s_scr = NULL;
    show(UI_SCR_HOME, NULL, LV_SCREEN_LOAD_ANIM_NONE);
    if (boot && boot != s_scr) {
        lv_obj_delete_delayed(boot, 100);
    }
}

void ui_update(const ui_model_t *model)
{
    if (!s_inited || !model) {
        return;
    }
    s_model = *model;

    ui_screen_t want = s_kind;
    switch (model->call) {
    case UI_CALL_DIALING:
    case UI_CALL_RINGING_OUT:
        want = UI_SCR_OUTGOING;
        break;
    case UI_CALL_RINGING_IN:
        want = UI_SCR_INCOMING; /* pops over anything, settings included */
        break;
    case UI_CALL_ACTIVE:
    case UI_CALL_HELD:
        want = UI_SCR_CALL;
        break;
    case UI_CALL_ENDED:
        want = UI_SCR_ENDED;
        break;
    case UI_CALL_IDLE:
    default:
        if (is_call_screen(s_kind)) {
            want = UI_SCR_HOME;
        }
        break;
    }

    if (want != s_kind) {
        show(want, NULL, LV_SCREEN_LOAD_ANIM_NONE);
    } else {
        ui_statusbar_update(&s_model);
        OPS[s_kind]->update(&s_model);
    }
}

void ui_key(char key, ui_key_action_t action)
{
    if (!s_inited) {
        return;
    }
    if (OPS[s_kind]->key) {
        OPS[s_kind]->key(key, action);
    }
}

void ui_touch_raw(int32_t ax, int32_t ay, bool pressed)
{
    if (s_inited && OPS[s_kind]->touch_raw) {
        OPS[s_kind]->touch_raw(ax, ay, pressed);
    }
}

/* ---- NULL-safe action wrappers ------------------------------------------- */

void ui_act_dial(const char *number)
{
    if (s_actions && s_actions->dial && number && number[0]) {
        s_actions->dial(s_actions->ctx, number);
    }
}

void ui_act_answer(void)
{
    if (s_actions && s_actions->answer) {
        s_actions->answer(s_actions->ctx);
    }
}

void ui_act_hangup(void)
{
    if (s_actions && s_actions->hangup) {
        s_actions->hangup(s_actions->ctx);
    }
}

void ui_act_dtmf(char digit)
{
    if (s_actions && s_actions->dtmf) {
        s_actions->dtmf(s_actions->ctx, digit);
    }
}

void ui_act_set_mute(bool muted)
{
    if (s_actions && s_actions->set_mute) {
        s_actions->set_mute(s_actions->ctx, muted);
    }
}

void ui_act_set_speaker(bool on)
{
    if (s_actions && s_actions->set_speaker) {
        s_actions->set_speaker(s_actions->ctx, on);
    }
}

void ui_act_set_volume(uint8_t volume)
{
    if (s_actions && s_actions->set_volume) {
        s_actions->set_volume(s_actions->ctx, volume);
    }
}

void ui_act_hold(bool hold)
{
    if (s_actions && s_actions->hold) {
        s_actions->hold(s_actions->ctx, hold);
    }
}

void ui_act_save_wifi(const char *ssid, const char *pass)
{
    if (s_actions && s_actions->save_wifi) {
        s_actions->save_wifi(s_actions->ctx, ssid, pass);
    }
}

void ui_act_save_account(const char *server, uint16_t port, const char *user,
                         const char *password, const char *display_name)
{
    if (s_actions && s_actions->save_account) {
        s_actions->save_account(s_actions->ctx, server, port, user, password, display_name);
    }
}

void ui_act_save_time(const char *ntp, const char *tz_posix, bool clock_24h)
{
    if (s_actions && s_actions->save_time && ntp && ntp[0] && tz_posix && tz_posix[0]) {
        s_actions->save_time(s_actions->ctx, ntp, tz_posix, clock_24h);
    }
}

void ui_act_ota_start(const char *url)
{
    if (s_actions && s_actions->ota_start && url && url[0]) {
        s_actions->ota_start(s_actions->ctx, url);
    }
}

void ui_act_save_blf(const char *list)
{
    if (s_actions && s_actions->save_blf && list) {
        s_actions->save_blf(s_actions->ctx, list);
    }
}

void ui_act_save_vm_number(const char *number)
{
    if (s_actions && s_actions->save_vm_number && number) {
        s_actions->save_vm_number(s_actions->ctx, number);
    }
}

/* ---- directory ---- */

size_t ui_act_contacts_search(const char *query, ui_contact_t *out, size_t max)
{
    if (s_actions && s_actions->contacts_search && out && max) {
        return s_actions->contacts_search(s_actions->ctx, query ? query : "", out, max);
    }
    return 0;
}

const char *ui_act_contact_save(const char *old_number, const char *name, const char *number,
                                uint8_t speed)
{
    if (!s_actions || !s_actions->contact_save) {
        return "Not available";
    }
    return s_actions->contact_save(s_actions->ctx, old_number, name, number, speed);
}

void ui_act_contact_delete(const char *number)
{
    if (s_actions && s_actions->contact_delete && number) {
        s_actions->contact_delete(s_actions->ctx, number);
    }
}

bool ui_act_speed_get(int digit, ui_contact_t *out)
{
    return s_actions && s_actions->speed_get && out && s_actions->speed_get(s_actions->ctx, digit, out);
}

size_t ui_act_history_list(ui_call_t *out, size_t max)
{
    if (s_actions && s_actions->history_list && out && max) {
        return s_actions->history_list(s_actions->ctx, out, max);
    }
    return 0;
}

void ui_act_history_clear(void)
{
    if (s_actions && s_actions->history_clear) {
        s_actions->history_clear(s_actions->ctx);
    }
}

void ui_act_history_mark_seen(void)
{
    if (s_actions && s_actions->history_mark_seen) {
        s_actions->history_mark_seen(s_actions->ctx);
    }
}

/* ---- sounds ---- */

const char *ui_act_ringtone_name(int idx)
{
    return s_actions && s_actions->ringtone_name ? s_actions->ringtone_name(s_actions->ctx, idx) : NULL;
}

void ui_act_ringtone_preview(int idx)
{
    if (s_actions && s_actions->ringtone_preview) {
        s_actions->ringtone_preview(s_actions->ctx, idx);
    }
}

void ui_act_save_sounds(uint8_t tone, uint8_t ring_vol, uint8_t call_vol, uint8_t spk_vol)
{
    if (s_actions && s_actions->save_sounds) {
        s_actions->save_sounds(s_actions->ctx, tone, ring_vol, call_vol, spk_vol);
    }
}

void ui_act_save_call_features(bool call_waiting, bool auto_answer, bool dnd)
{
    if (s_actions && s_actions->save_call_features) {
        s_actions->save_call_features(s_actions->ctx, call_waiting, auto_answer, dnd);
    }
}

void ui_act_touch_cal_begin(void)
{
    if (s_actions && s_actions->touch_cal_begin) {
        s_actions->touch_cal_begin(s_actions->ctx);
    }
}

void ui_act_touch_cal_end(const ui_touch_cal_t *cal_or_null)
{
    if (s_actions && s_actions->touch_cal_end) {
        s_actions->touch_cal_end(s_actions->ctx, cal_or_null);
    }
}

/* ---- shared helpers -------------------------------------------------------- */

void ui_remote_texts(const ui_model_t *m, const char **main_txt, const char **sub_txt)
{
    if (m->remote_name[0]) {
        *main_txt = m->remote_name;
        *sub_txt = m->remote_number;
    } else {
        *main_txt = m->remote_number[0] ? m->remote_number : "Unknown";
        *sub_txt = "";
    }
}

bool ui_is_dial_key(char key)
{
    return (key >= '0' && key <= '9') || key == '*' || key == '#';
}
