#pragma once

/* User intents raised by the UI. Implemented by `phone` on device and by the
 * simulator's mock. Callbacks run in the LVGL task with the lock held: they
 * must not block — post to a queue/event and return. Any member may be NULL. */

#include <stdbool.h>
#include <stdint.h>

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
    void (*save_wifi)(void *ctx, const char *ssid, const char *pass);
    void (*save_account)(void *ctx, const char *server, uint16_t port,
                         const char *user, const char *password,
                         const char *display_name);
} ui_actions_t;
