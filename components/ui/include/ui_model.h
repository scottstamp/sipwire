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

/* call features: transfer progress (mirrors the phone's xfer_state_t) */
typedef enum {
    UI_XFER_IDLE,
    UI_XFER_CONSULT,           /* attended: consultation call placed, original on hold */
    UI_XFER_TRYING,            /* REFER sent */
    UI_XFER_ACCEPTED,
    UI_XFER_PROGRESS,          /* target is being called */
    UI_XFER_DONE,              /* transferred; the calls are hung up */
    UI_XFER_FAILED,            /* xfer_code says why; the call is kept */
} ui_xfer_state_t;

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

/* ---- directory (contacts, call history) ---- */

#define UI_CONTACTS_MAX   200  /* directory.h DIR_CONTACTS_MAX */
#define UI_HISTORY_MAX    100  /* directory.h DIR_HISTORY_MAX */
#define UI_NAME_LEN       32   /* incl. NUL */
#define UI_NUMBER_LEN     32   /* incl. NUL */
#define UI_SPEED_FIRST    2    /* speed dial keys 2..9 */
#define UI_SPEED_LAST     9

typedef struct {
    char name[UI_NAME_LEN];
    char number[UI_NUMBER_LEN];
    uint8_t speed;             /* speed dial key 2..9, 0 = none */
} ui_contact_t;

typedef enum { UI_CALLDIR_IN, UI_CALLDIR_OUT } ui_call_dir_t;
typedef enum {
    UI_CALLRES_ANSWERED,
    UI_CALLRES_MISSED,         /* incoming: not answered; outgoing: no answer / failed */
    UI_CALLRES_REJECTED,       /* declined by the user */
    UI_CALLRES_DND,            /* refused by do-not-disturb */
} ui_call_result_t;

typedef struct {
    uint8_t direction;         /* ui_call_dir_t */
    uint8_t result;            /* ui_call_result_t */
    bool seen;                 /* missed calls: false until the history was opened */
    int64_t start;             /* seconds since the epoch, 0 = unknown */
    uint32_t duration_s;
    char number[UI_NUMBER_LEN];
    char name[UI_NAME_LEN];    /* may be empty */
} ui_call_t;

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

    /* call features. The call fields above describe the foreground call; the
     * background call is a waiting incoming call (UI_CALL_RINGING_IN) or a
     * held one (UI_CALL_HELD); UI_CALL_IDLE = none. */
    bool dnd;                  /* do not disturb on */
    bool auto_answered;        /* foreground call was answered automatically (page / intercom) */
    ui_call_state_t second_call;
    char second_number[64];
    char second_name[64];      /* may be empty */
    uint32_t second_seconds;   /* elapsed while held */
    ui_xfer_state_t xfer;      /* transfer in progress / last result (until the phone is idle) */
    bool xfer_attended;
    int xfer_code;             /* SIP status for UI_XFER_FAILED / DONE, 0 = none */
    char xfer_target[64];
    /* directory: the lists themselves are fetched through ui_actions_t (contacts_search,
     * history_list) when a screen opens; only the badge count lives in the model. */
    uint8_t missed_unseen;     /* missed incoming calls not yet seen in the history (capped at 99) */
    char missed_last[UI_NAME_LEN]; /* name (else number) of the newest of those; "" if none */
    uint32_t history_rev;      /* changes whenever the call history does (new call, clear, seen):
                                * an open History screen refetches its list */
    /* incoming call refused without ringing (do not disturb, or busy without call waiting):
     * missed_seq counts up once per call, the UI shows a short notice */
    uint16_t missed_seq;
    bool missed_dnd;           /* that call was refused because of do not disturb */
    char missed_from[64];      /* its caller: name, else number */
    int64_t now;               /* wall clock, seconds since the epoch rounded down to the minute;
                                * 0 until the clock is set */

    /* sounds + call features (stored settings; prefill Settings > Sounds / Call features) */
    uint8_t ringtone_count;    /* ui_actions_t.ringtone_name valid for 0..count-1 */
    uint8_t ring_tone;
    uint8_t ring_volume;       /* 0-100 */
    uint8_t call_volume;
    uint8_t speaker_volume;
    bool call_waiting;
    bool auto_answer;          /* answer pages / intercom calls automatically */
    /* dnd: see the call features block above */

    /* locks (ui_lock_logic.h). In-call touch lock: on the handset, an answered call's screen
     * ignores touches this many seconds after it connected / the last tap or key (Kconfig
     * SIPWIRE_CALL_TOUCH_LOCK_S; 0 = off). Keypad lock: locked by holding * on the home screen
     * or its lock button, and with keypad_autolock also when the display goes dark
     * (ui_display_off). */
    uint16_t call_touch_lock_s;
    bool keypad_autolock;
} ui_model_t;
