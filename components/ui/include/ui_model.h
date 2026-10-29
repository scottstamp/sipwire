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

typedef enum {
    UI_OTA_IDLE,
    UI_OTA_RUNNING,            /* connecting / downloading / verifying */
    UI_OTA_DONE,               /* installed, device reboots shortly */
    UI_OTA_FAILED,             /* ota_message says why */
} ui_ota_state_t;

typedef enum {
    UI_BLF_UNKNOWN,            /* not subscribed / failed: grey lamp */
    UI_BLF_IDLE,               /* green */
    UI_BLF_RINGING,            /* blinking red/amber */
    UI_BLF_BUSY,               /* red */
} ui_blf_state_t;

#define UI_BLF_MAX       8
#define UI_BLF_EXT_LEN   32    /* incl. NUL */
#define UI_BLF_LABEL_LEN 24    /* incl. NUL */

typedef struct {
    char ext[UI_BLF_EXT_LEN];      /* dialled when the tile is tapped */
    char label[UI_BLF_LABEL_LEN];  /* shown instead of ext when set */
    ui_blf_state_t state;
} ui_blf_t;

/* Longest preformatted clock text: "12:34 PM" (12 h, no leading zero) or "03:07"
 * (24 h); "" until the clock is set. The home screen splits off the AM/PM at the space. */
#define UI_TIME_STR_LEN 12

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
    char time_hhmm[UI_TIME_STR_LEN]; /* "3:07 PM" / "15:07" per clock_24h; "" until SNTP sync */

    /* Stored settings that prefill the Settings forms. No secrets: the
     * password fields only learn whether one is set. */
    char wifi_ssid[33];
    bool wifi_pass_set;
    char sip_server[64];
    uint16_t sip_port;         /* 0 = unset */
    char sip_user[64];
    char sip_display_name[64];
    bool sip_pass_set;

    /* Time settings (Settings > Time) */
    char ntp_server[64];
    char tz[64];               /* POSIX TZ string */
    bool clock_24h;            /* false = 12-hour with AM/PM */

    /* Firmware (Settings > Update) */
    char fw_version[32];
    char fw_partition[16];     /* running slot, e.g. "ota_0" */
    char ota_url[192];         /* stored update URL (prefill) */
    ui_ota_state_t ota_state;
    uint8_t ota_percent;       /* 0-100 */
    char ota_message[64];      /* status line, e.g. "Downloading 42%"; may be empty */

    /* BLF keys (settings `blf`, in order) and message waiting */
    uint8_t blf_count;         /* 0 = no BLF panel, home screen as without */
    ui_blf_t blf[UI_BLF_MAX];
    bool mwi_waiting;          /* envelope in the status bar, voicemail button */
    uint16_t mwi_new;          /* new / old voice messages (0 if unknown) */
    uint16_t mwi_old;
    char vm_number[32];        /* dialled by the voicemail button ("*97"); may be empty */
} ui_model_t;
