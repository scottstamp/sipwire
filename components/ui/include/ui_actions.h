#pragma once

/* User intents raised by the UI. Implemented by `phone` on device and by the
 * simulator's mock. Callbacks run in the LVGL task with the lock held: they
 * must not block — post to a queue/event and return. Any member may be NULL. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ui_model.h"

/* Touch calibration, same semantics as settings.h `touch_cal` and display.h:
 * raw 12-bit touch values (after the controller's swap_xy, before any mirror or
 * calibration) measured at screen x = 0 / x = width-1 and y = 0 / y = height-1.
 * min > max is allowed (inverted axis). */
typedef struct {
    int16_t x_min, x_max, y_min, y_max;
} ui_touch_cal_t;

typedef struct {
    void *ctx;
    void (*dial)(void *ctx, const char *number);
    void (*answer)(void *ctx);
    void (*hangup)(void *ctx);           /* also rejects an incoming call */
    void (*dtmf)(void *ctx, char digit); /* during an active call */
    void (*set_mute)(void *ctx, bool muted);
    void (*set_speaker)(void *ctx, bool on);
    void (*set_volume)(void *ctx, uint8_t volume);
    void (*hold)(void *ctx, bool hold);
    /* An empty password keeps the stored one (the forms never show it). */
    void (*save_wifi)(void *ctx, const char *ssid, const char *pass);
    /* Likewise: an empty password keeps the stored one. */
    void (*save_account)(void *ctx, const char *server, uint16_t port,
                         const char *user, const char *password,
                         const char *display_name);
    /* The calibration screen opened: from now until touch_cal_end the device
     * must stop delivering touches to LVGL and instead feed raw samples to
     * ui_touch_raw(). */
    void (*touch_cal_begin)(void *ctx);
    /* The calibration screen closed (called once per begin, also when the
     * screen is left because of an incoming call). `cal` is the result to
     * store, or NULL when cancelled. Persist off the LVGL task. */
    void (*touch_cal_end)(void *ctx, const ui_touch_cal_t *cal);
    /* Settings > Time: NTP server, POSIX TZ string and 12/24-hour display.
     * Applied live (no reboot); the strings are never empty. */
    void (*save_time)(void *ctx, const char *ntp, const char *tz_posix, bool clock_24h);
    /* Settings > Update: install the firmware at `url` (http:// or https://).
     * The device stores the URL if it changed. Progress comes back through the
     * model's ota_* fields. Not called while a call is active. */
    void (*ota_start)(void *ctx, const char *url);
    /* Settings > BLF keys: the whole list "ext[:label],..." (at most
     * UI_BLF_MAX entries, extensions unique, no ',' or ':' in labels; "" =
     * none) replaces the stored one; subscriptions follow live. */
    void (*save_blf)(void *ctx, const char *list);
    /* Settings > BLF keys: number the voicemail button dials ("" = none). */
    void (*save_vm_number)(void *ctx, const char *number);

    /* call features (any member may be NULL) */
    void (*answer_waiting)(void *ctx);   /* hold the current call, answer the waiting one */
    void (*reject_waiting)(void *ctx);   /* refuse the waiting call */
    void (*swap)(void *ctx);             /* hold the foreground call, resume the held one */
    void (*transfer_blind)(void *ctx, const char *target);
    void (*transfer_attended_start)(void *ctx, const char *target);  /* hold + consult call */
    void (*transfer_attended_complete)(void *ctx);
    void (*transfer_attended_cancel)(void *ctx);
    void (*set_dnd)(void *ctx, bool on);
    /* ---- directory (called from the LVGL task: in-RAM lookups and changes; the saves update RAM at
     * once, so the screen can refetch right after, and queue the flash write: they never block) ---- */
    /* Contacts whose name contains `query` or whose number contains it (case-insensitive;
     * "" = all), best matches first, then sorted. Fills up to `max` entries incl. the
     * speed dial key of each; returns the count. */
    size_t (*contacts_search)(void *ctx, const char *query, ui_contact_t *out, size_t max);
    /* Add or change a contact. `old_number` (NULL for a new one) is the number the contact
     * had before, so a changed number replaces it. `speed` 2..9 assigns that speed dial key
     * to the contact (0 = none). Returns NULL on success or a short error text. */
    const char *(*contact_save)(void *ctx, const char *old_number, const char *name,
                                const char *number, uint8_t speed);
    /* Delete a contact and its speed dial key. */
    void (*contact_delete)(void *ctx, const char *number);
    /* Speed dial slot `digit` (2..9): fills `out` (number, and the contact's name if known)
     * and returns true when assigned. */
    bool (*speed_get)(void *ctx, int digit, ui_contact_t *out);
    /* Call history, newest first; returns the count (<= max). */
    size_t (*history_list)(void *ctx, ui_call_t *out, size_t max);
    void (*history_clear)(void *ctx);
    /* Mark all missed calls seen (the badge count follows in the model). */
    void (*history_mark_seen)(void *ctx);

    /* ---- sounds / call features ---- */
    const char *(*ringtone_name)(void *ctx, int idx);   /* NULL out of range */
    /* Play ringtone `idx` in a loop (preview), or stop with idx < 0. Stopping does nothing
     * while a call rings or is active (the phone owns the speaker then). */
    void (*ringtone_preview)(void *ctx, int idx);
    /* Persist ringtone + volumes (0-100). */
    void (*save_sounds)(void *ctx, uint8_t ring_tone, uint8_t ring_volume, uint8_t call_volume,
                        uint8_t speaker_volume);
    /* Persist the call feature switches (applied live). */
    void (*save_call_features)(void *ctx, bool call_waiting, bool auto_answer, bool dnd);
    /* Persist the quiet hours (start / end: minutes after midnight 0-1439). */
    void (*save_quiet)(void *ctx, bool on, uint16_t start_min, uint16_t end_min, bool allow_contacts);
} ui_actions_t;
