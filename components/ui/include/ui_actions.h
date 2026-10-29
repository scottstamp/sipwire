#pragma once

/* User intents raised by the UI. Implemented by `phone` on device and by the
 * simulator's mock. Callbacks run in the LVGL task with the lock held: they
 * must not block — post to a queue/event and return. Any member may be NULL. */

#include <stdbool.h>
#include <stdint.h>

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
} ui_actions_t;
