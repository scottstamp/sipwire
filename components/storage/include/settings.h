#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "sipwire_blf.h"

typedef enum {
    SIP_TRANSPORT_UDP = 0,
    SIP_TRANSPORT_TCP = 1,
} sip_transport_t;

typedef struct {
    bool valid;
    int16_t x_min, x_max, y_min, y_max;
} settings_touch_cal_t;

#define SETTINGS_AEC_SUPPRESS_MAX  90  /* dB, the canceller's limit (AEC_SUPPRESS_MIN_DB) */
#define SETTINGS_MINUTES_PER_DAY   1440    /* quiet_start / quiet_end: 0..1439 */
#define SETTINGS_FWD_NOANSWER_MIN  5       /* fwd_noanswer_s range, seconds */
#define SETTINGS_FWD_NOANSWER_MAX  300

typedef struct {
    /* wifi */
    char wifi_ssid[33];
    char wifi_pass[65];
    /* sip account */
    char sip_server[64];       /* host or IP of registrar / proxy */
    uint16_t sip_port;         /* default 5060 */
    sip_transport_t sip_transport;
    char sip_user[64];         /* AOR user part */
    char sip_auth_user[64];    /* empty = same as sip_user */
    char sip_password[64];
    char sip_display_name[64];
    uint32_t sip_reg_expires;  /* seconds, default 300 */
    char codec_pref[24];       /* "g722,pcmu,pcma" (default); "" = default */
    /* BLF keys + voicemail */
    char blf[SIPWIRE_BLF_LIST_MAX]; /* "1002:Bob,1003:Front desk" (sipwire_blf.h), "" = none */
    char vm_number[32];        /* dialled by the voicemail button, default "*97" */
    /* call features */
    bool call_waiting;         /* offer a second incoming call during a call, default true */
    bool dnd;                  /* do not disturb: refuse incoming calls with 486, default false */
    bool auto_answer;          /* answer pages / intercom calls (Call-Info answer-after...), default true */
    char find_alert[32];       /* Alert-Info marker of a find-the-handset call (rings at full volume
                                * through DND and quiet hours), default "find"; "" = off; SIP token */
    bool quiet_hours;          /* quiet hours on (only with the clock set), default false */
    uint16_t quiet_start;      /* minutes after midnight 0-1439, default 22:00 (1320) */
    uint16_t quiet_end;        /* default 07:00 (420); end <= start wraps past midnight; equal = never */
    bool quiet_allow_contacts; /* callers in the contacts still ring in quiet hours, default true */
    char fwd_always[32];       /* forward every incoming call (302) here; "" = off. Number or sip: URI */
    char fwd_busy[32];         /* ... when busy (no free line) or declined; "" = off */
    char fwd_noanswer[32];     /* ... when not answered within fwd_noanswer_s; "" = off */
    uint16_t fwd_noanswer_s;   /* 5-300, default 20 */
    /* audio */
    uint8_t ring_volume;       /* 0-100 */
    uint8_t call_volume;       /* 0-100 */
    uint8_t mic_gain;          /* 0-100 */
    bool aec;                  /* acoustic echo canceller on the mic path, default false */
    uint8_t aec_suppress;      /* residual echo attenuation, dB (0-90), far end alone; default 40 */
    uint8_t aec_suppress_act;  /* same while both talk; default 15; both 0 = linear filter only */
    /* display */
    uint8_t backlight;         /* 0-100 */
    uint16_t backlight_timeout_s;
    /* clock / time */
    char ntp_server[64];       /* default CONFIG_SIPWIRE_NTP_SERVER; never empty */
    char tz[64];               /* POSIX TZ string, default CONFIG_SIPWIRE_TZ; never empty */
    bool clock_24h;            /* false (default) = 12-hour display with AM/PM */
    /* firmware update */
    char ota_url[192];         /* default CONFIG_SIPWIRE_OTA_URL; "" = none */
    /* ringer / loudspeaker (block: ring_volume and call_volume are in "audio" above) */
    uint8_t ring_tone;         /* ringtone index (audio_ringtone_name), default 0; unknown = 0 */
    uint8_t speaker_volume;    /* 0-100 loudspeaker-mode call volume, default 90 */
    /* touch calibration (raw XPT2046 → screen). valid=false → uncalibrated */
    settings_touch_cal_t touch_cal;
} settings_t;

/** Load from NVS (namespace "sipwire"), falling back to defaults per field. */
esp_err_t settings_init(void);

/** Snapshot copy; safe from any task. */
void settings_get(settings_t *out);

/** Validate, persist to NVS, post SETTINGS_EVENT_CHANGED. */
esp_err_t settings_set(const settings_t *in);

/** Copy `len` bytes at byte offset `off` of the current settings to `dst`; safe from any task.
 *  Use SETTINGS_GET(). */
void settings_get_field(size_t off, void *dst, size_t len);

/** settings_set() of the current settings with `len` bytes at offset `off` replaced by `src`,
 *  atomically (under the settings lock). Use SETTINGS_SET(). */
esp_err_t settings_set_field(size_t off, const void *src, size_t len);

/* One field without a settings_t (~1.3 KB) on the caller's stack. `ptr` points to an object of
 * the field's size (checked at compile time):
 *     uint8_t tone;
 *     SETTINGS_GET(ring_tone, &tone);
 *     char url[SETTINGS_FIELD_SIZE(ota_url)];
 *     SETTINGS_GET(ota_url, &url);
 *     esp_err_t err = SETTINGS_SET(dnd, &on);
 * Each call is one snapshot: fields read with separate calls may straddle a settings_set(),
 * which is followed by SETTINGS_EVENT_CHANGED. */
#define SETTINGS_FIELD_SIZE(field) sizeof(((settings_t *)0)->field)
#define SETTINGS_CHECKED_SIZE_(field, ptr) \
    (sizeof(*(ptr)) + 0 * sizeof(char[sizeof(*(ptr)) == SETTINGS_FIELD_SIZE(field) ? 1 : -1]))
#define SETTINGS_GET(field, ptr) \
    settings_get_field(offsetof(settings_t, field), (ptr), SETTINGS_CHECKED_SIZE_(field, ptr))
#define SETTINGS_SET(field, ptr) \
    settings_set_field(offsetof(settings_t, field), (ptr), SETTINGS_CHECKED_SIZE_(field, ptr))

/** A forwarding target (fwd_*): "" (off), a dialable number (0-9 A-Z a-z * # + . _ -) or a
 *  "sip:" URI without spaces, quotes or angle brackets. */
bool settings_fwd_target_ok(const char *s);

/** Restore defaults (keeps nothing), persist, post SETTINGS_EVENT_CHANGED. */
esp_err_t settings_reset(void);
