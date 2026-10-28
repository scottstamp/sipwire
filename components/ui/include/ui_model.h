#pragma once

/* Snapshot of everything the UI renders. Pure C, no IDF headers: the same
 * struct is filled by `phone` on device and by the web simulator's mock. */

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    UI_NET_OFF,
    UI_NET_CONNECTING,
    UI_NET_CONNECTED,
} ui_net_state_t;

typedef enum {
    UI_REG_NONE,
    UI_REG_REGISTERING,
    UI_REG_REGISTERED,
    UI_REG_FAILED,
} ui_reg_state_t;

typedef enum {
    UI_CALL_IDLE,
    UI_CALL_DIALING,
    UI_CALL_RINGING_OUT,
    UI_CALL_RINGING_IN,
    UI_CALL_ACTIVE,
    UI_CALL_HELD,
    UI_CALL_ENDED,
} ui_call_state_t;

typedef struct {
    ui_net_state_t net;
    int8_t rssi;               /* dBm, valid when net == UI_NET_CONNECTED */
    char ip[16];

    ui_reg_state_t reg;
    char account[64];          /* "Display Name <user@server>" or "user" */

    ui_call_state_t call;
    char remote_number[64];
    char remote_name[64];      /* may be empty */
    uint32_t call_seconds;     /* elapsed in UI_CALL_ACTIVE / HELD */
    int end_code;              /* SIP status for UI_CALL_ENDED, 0 = normal */
    bool muted;
    bool speaker;              /* loud speaker vs handset level */

    uint8_t volume;            /* 0-100 current output (ring or call) */
    int8_t battery_pct;        /* -1 = no battery */
    bool charging;
    char time_hhmm[6];         /* "" until SNTP sync */
} ui_model_t;
