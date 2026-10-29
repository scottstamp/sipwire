#pragma once

/* App-wide events, posted on the default esp_event loop.
 * Add new IDs at the end of an enum; payload structs are copied by esp_event. */

#include <stdbool.h>
#include <stdint.h>
#include "esp_event.h"

ESP_EVENT_DECLARE_BASE(SIPWIRE_NET_EVENT);
ESP_EVENT_DECLARE_BASE(SIPWIRE_SIP_EVENT);
ESP_EVENT_DECLARE_BASE(SIPWIRE_CALL_EVENT);
ESP_EVENT_DECLARE_BASE(SIPWIRE_KEY_EVENT);
ESP_EVENT_DECLARE_BASE(SIPWIRE_SETTINGS_EVENT);
ESP_EVENT_DECLARE_BASE(SIPWIRE_OTA_EVENT);
ESP_EVENT_DECLARE_BASE(SIPWIRE_BLF_EVENT);

/* SIPWIRE_NET_EVENT */
typedef enum {
    NET_EVENT_CONNECTING,      /* no payload */
    NET_EVENT_GOT_IP,          /* net_event_ip_t */
    NET_EVENT_DISCONNECTED,    /* net_event_disc_t */
    NET_EVENT_RSSI,            /* int8_t, periodic while connected */
} net_event_id_t;

typedef struct {
    uint32_t ip;               /* network byte order */
    char ip_str[16];
} net_event_ip_t;

typedef struct {
    uint8_t reason;            /* wifi_err_reason_t */
} net_event_disc_t;

/* SIPWIRE_SIP_EVENT — registration, message waiting (emitted by phone) */
typedef enum {
    SIP_EVENT_REG_STATE,       /* sip_event_reg_t */
    SIP_EVENT_MWI,             /* sip_event_mwi_t, when it changes */
} sip_event_id_t;

typedef enum {
    SIP_REG_NONE,
    SIP_REG_REGISTERING,
    SIP_REG_REGISTERED,
    SIP_REG_FAILED,
} sip_reg_state_t;

typedef struct {
    sip_reg_state_t state;
    int status_code;           /* last SIP response, 0 if none / timeout */
} sip_event_reg_t;

typedef struct {
    bool known;                /* false: no MWI information (yet / any more) */
    bool waiting;              /* Messages-Waiting: yes */
    uint16_t new_msgs;         /* new voice messages (capped at 9999) */
    uint16_t old_msgs;
} sip_event_mwi_t;

/* SIPWIRE_CALL_EVENT — call lifecycle (emitted by phone) */
typedef enum {
    CALL_EVENT_STATE,          /* call_event_state_t */
} call_event_id_t;

typedef enum {
    CALL_IDLE,
    CALL_DIALING,              /* outgoing INVITE sent, no provisional yet */
    CALL_RINGING_OUT,          /* 180/183 received */
    CALL_RINGING_IN,           /* incoming INVITE, ringing locally */
    CALL_ACTIVE,
    CALL_HELD,
    CALL_ENDED,                /* transient; reason in end_code */
} call_state_t;

typedef struct {
    call_state_t state;
    char number[64];
    char name[64];
    int end_code;              /* SIP status for CALL_ENDED, 0 = normal hangup */
} call_event_state_t;

/* SIPWIRE_KEY_EVENT — physical keypad */
typedef enum {
    KEY_EVENT_PRESS,           /* key_event_t */
    KEY_EVENT_RELEASE,
    KEY_EVENT_LONG,            /* held > long-press threshold, once */
    KEY_EVENT_REPEAT,          /* while held after LONG */
} key_event_id_t;

typedef struct {
    char key;                  /* '0'-'9', '*', '#' */
} key_event_t;

/* SIPWIRE_SETTINGS_EVENT */
typedef enum {
    SETTINGS_EVENT_CHANGED,    /* no payload; re-read via settings_get() */
} settings_event_id_t;

/* SIPWIRE_OTA_EVENT — firmware update (emitted by ota) */
typedef enum {
    OTA_EVENT_STARTED,         /* no payload; connecting / checking the image */
    OTA_EVENT_PROGRESS,        /* ota_event_progress_t, whenever the percentage changes */
    OTA_EVENT_DONE,            /* no payload; image written and verified, reboot in ~3 s */
    OTA_EVENT_FAILED,          /* ota_event_failed_t; nothing was activated */
} ota_event_id_t;

typedef struct {
    uint8_t percent;           /* 0-100, 0 while the image size is unknown */
    uint32_t bytes;            /* image bytes written so far */
} ota_event_progress_t;

typedef struct {
    int32_t err;               /* esp_err_t */
    char reason[64];           /* short human readable cause */
} ota_event_failed_t;

/* SIPWIRE_BLF_EVENT — busy lamp field (emitted by phone). Entries follow the
 * `blf` setting (sipwire_blf.h), index 0..SIPWIRE_BLF_MAX-1. After the list
 * changes every entry is posted once (with its current state). */
typedef enum {
    BLF_EVENT_STATE,           /* blf_event_state_t */
} blf_event_id_t;

typedef enum {
    BLF_UNKNOWN,               /* not subscribed (yet), refused or failed */
    BLF_IDLE,
    BLF_RINGING,
    BLF_BUSY,
} blf_state_t;

typedef struct {
    uint8_t index;
    char ext[32];              /* SIPWIRE_BLF_EXT_MAX */
    blf_state_t state;
} blf_event_state_t;
