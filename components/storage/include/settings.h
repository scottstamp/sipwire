#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef enum {
    SIP_TRANSPORT_UDP = 0,
    SIP_TRANSPORT_TCP = 1,
} sip_transport_t;

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
    /* audio */
    uint8_t ring_volume;       /* 0-100 */
    uint8_t call_volume;       /* 0-100 */
    uint8_t mic_gain;          /* 0-100 */
    /* display */
    uint8_t backlight;         /* 0-100 */
    uint16_t backlight_timeout_s;
    /* clock / time */
    char ntp_server[64];       /* default CONFIG_SIPWIRE_NTP_SERVER; never empty */
    char tz[64];               /* POSIX TZ string, default CONFIG_SIPWIRE_TZ; never empty */
    bool clock_24h;            /* false (default) = 12-hour display with AM/PM */
    /* firmware update */
    char ota_url[192];         /* default CONFIG_SIPWIRE_OTA_URL; "" = none */
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
