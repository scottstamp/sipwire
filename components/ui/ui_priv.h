#pragma once

/* Private to components/ui: screen router interface shared by the screens. */

#include "ui.h"
#include "ui_theme.h"

typedef enum {
    UI_SCR_HOME,
    UI_SCR_DIALER,
    UI_SCR_OUTGOING, /* DIALING / RINGING_OUT */
    UI_SCR_INCOMING, /* RINGING_IN */
    UI_SCR_CALL,     /* ACTIVE / HELD */
    UI_SCR_ENDED,
    UI_SCR_SETTINGS,
    UI_SCR_CALIBRATE,
    UI_SCR_TRANSFER, /* call features: pick a transfer target */
    UI_SCR_COUNT
} ui_screen_t;

/* Every screen module provides these. `create` builds the screen (`arg` is a
 * screen specific hint, e.g. the first dialed character), `update` applies a
 * model snapshot to the live screen, `key` handles a physical key. Optional:
 * `touch_raw` receives raw touch samples (calibration only), `leave` runs
 * right before another screen replaces this one (not on deferred deletion). */
typedef struct {
    lv_obj_t *(*create)(const char *arg);
    void (*update)(const ui_model_t *m);
    void (*key)(char key, ui_key_action_t action);
    void (*touch_raw)(int32_t ax, int32_t ay, bool pressed);
    void (*leave)(void);
} ui_screen_ops_t;

extern const ui_screen_ops_t ui_ops_home;
extern const ui_screen_ops_t ui_ops_dialer;
extern const ui_screen_ops_t ui_ops_outgoing;
extern const ui_screen_ops_t ui_ops_incoming;
extern const ui_screen_ops_t ui_ops_call;
extern const ui_screen_ops_t ui_ops_ended;
extern const ui_screen_ops_t ui_ops_settings;
extern const ui_screen_ops_t ui_ops_calibrate;
extern const ui_screen_ops_t ui_ops_transfer;

/** User initiated navigation (animated). Safe to call from event callbacks. */
void ui_goto(ui_screen_t screen, const char *arg);

/** Latest model snapshot. */
const ui_model_t *ui_model(void);

/* ---- status bar ---- */
lv_obj_t *ui_statusbar_create(lv_obj_t *screen);
void ui_statusbar_update(const ui_model_t *m);

/* ---- actions: NULL-safe wrappers around ui_actions_t ---- */
void ui_act_dial(const char *number);
void ui_act_answer(void);
void ui_act_hangup(void);
void ui_act_dtmf(char digit);
void ui_act_set_mute(bool muted);
void ui_act_set_speaker(bool on);
void ui_act_set_volume(uint8_t volume);
void ui_act_hold(bool hold);
void ui_act_save_wifi(const char *ssid, const char *pass);
void ui_act_save_account(const char *server, uint16_t port, const char *user,
                         const char *password, const char *display_name);
void ui_act_save_time(const char *ntp, const char *tz_posix, bool clock_24h);
void ui_act_ota_start(const char *url);
void ui_act_save_blf(const char *list);
void ui_act_save_vm_number(const char *number);
void ui_act_touch_cal_begin(void);
void ui_act_touch_cal_end(const ui_touch_cal_t *cal_or_null);

/* call features */
void ui_act_answer_waiting(void);
void ui_act_reject_waiting(void);
void ui_act_swap(void);
void ui_act_transfer_blind(const char *target);
void ui_act_transfer_attended_start(const char *target);
void ui_act_transfer_attended_complete(void);
void ui_act_transfer_attended_cancel(void);
void ui_act_set_dnd(bool on);

/** Display name of a transfer target: the BLF label for `ext`, else `ext`. */
const char *ui_target_name(const ui_model_t *m, const char *ext);

/** Text for the transfer state ("Transferring... trying", "Transfer failed: 486") or NULL
 *  for UI_XFER_IDLE / CONSULT. `bad` is set for a failure. */
const char *ui_xfer_text(const ui_model_t *m, char *buf, size_t cap, bool *bad);

/* ---- helpers shared by call screens ---- */

/** Show name (or number) as the main line and the number as a sub line. */
void ui_remote_texts(const ui_model_t *m, const char **main_txt, const char **sub_txt);

/** True for keys that carry a dialable character. */
bool ui_is_dial_key(char key);
