#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "sipwire_blf.h"

typedef enum {
    SIP_TRANSPORT_UDP = 0,
    SIP_TRANSPORT_TCP = 1,
} sip_transport_t;

#define SETTINGS_AEC_SUPPRESS_MAX  90  /* dB, the canceller's limit (AEC_SUPPRESS_MIN_DB) */

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
    struct {
        bool valid;
        int16_t x_min, x_max, y_min, y_max;
    } touch_cal;
} settings_t;

/** Load from NVS (namespace "sipwire"), falling back to defaults per field. */
esp_err_t settings_init(void);

/** Snapshot copy; safe from any task. */
void settings_get(settings_t *out);

/** Validate, persist to NVS, post SETTINGS_EVENT_CHANGED. */
esp_err_t settings_set(const settings_t *in);

/** Restore defaults (keeps nothing), persist, post SETTINGS_EVENT_CHANGED. */
esp_err_t settings_reset(void);
