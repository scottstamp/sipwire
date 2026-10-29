#pragma once

/* Private to components/ui: screen router interface shared by the screens. */

#include "ui.h"
#include "ui_fmt.h"
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
    UI_SCR_HISTORY,
    UI_SCR_CONTACTS,
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
extern const ui_screen_ops_t ui_ops_history;
extern const ui_screen_ops_t ui_ops_contacts;

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
/* directory */
size_t ui_act_contacts_search(const char *query, ui_contact_t *out, size_t max);
const char *ui_act_contact_save(const char *old_number, const char *name, const char *number,
                                uint8_t speed);   /* NULL = ok, else error text */
void ui_act_contact_delete(const char *number);
bool ui_act_speed_get(int digit, ui_contact_t *out);
size_t ui_act_history_list(ui_call_t *out, size_t max);
void ui_act_history_clear(void);
void ui_act_history_mark_seen(void);
/* sounds */
const char *ui_act_ringtone_name(int idx);
void ui_act_ringtone_preview(int idx);   /* idx < 0 stops */
void ui_act_save_sounds(uint8_t tone, uint8_t ring_vol, uint8_t call_vol, uint8_t spk_vol);
void ui_act_save_call_features(bool call_waiting, bool auto_answer, bool dnd);
void ui_act_touch_cal_begin(void);
void ui_act_touch_cal_end(const ui_touch_cal_t *cal_or_null);

/* ---- helpers shared by call screens ---- */

/** Show name (or number) as the main line and the number as a sub line. */
void ui_remote_texts(const ui_model_t *m, const char **main_txt, const char **sub_txt);

/** Virtual list: a scrollable column of `row_h` high rows of which only the visible ones
 *  (plus two) exist as objects; they are re-bound to the items scrolled into view. For
 *  lists of up to a few hundred entries without a few hundred widgets (ui_vlist.c). */
typedef struct {
    /* Build the children of one pool row (called once per pool row). */
    void (*make_row)(lv_obj_t *row, void *user);
    /* Fill `row` for item `idx`. */
    void (*bind_row)(lv_obj_t *row, int32_t idx, void *user);
    /* The row of item `idx` was tapped. */
    void (*clicked)(int32_t idx, void *user);
} ui_vlist_ops_t;

lv_obj_t *ui_vlist_create(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h, int32_t row_h,
                          const ui_vlist_ops_t *ops, void *user);
/** New item count; keeps the scroll position when possible. Rebinds the rows. */
void ui_vlist_set_count(lv_obj_t *list, int32_t count);
/** Rebind the visible rows after the data changed. */
void ui_vlist_refresh(lv_obj_t *list);
/** Resize (e.g. when the keyboard appears). */
void ui_vlist_set_height(lv_obj_t *list, int32_t h);
/** The item index of a pool row (-1 if unbound). */
int32_t ui_vlist_row_index(lv_obj_t *row);

/** True for keys that carry a dialable character. */
bool ui_is_dial_key(char key);
