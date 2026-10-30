#pragma once

/* Pure part of the UI bridge: builds the ui_model_t snapshot from the
 * phone status and the bridge's own net/clock/account state. No FreeRTOS or
 * LVGL calls, host-tested in test_host/phone. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ota.h"
#include "phone.h"
#include "ui_model.h"

typedef struct {
    ui_net_state_t net;
    int8_t rssi;
    char ip[16];
    char account[64];
    /* non-secret settings for the Settings forms; passwords only as flags */
    char wifi_ssid[33];
    bool wifi_pass_set;
    char sip_server[64];
    uint16_t sip_port;
    char sip_user[64];
    char sip_display_name[64];
    bool sip_pass_set;
    bool time_valid;                /* clock set by SNTP */
    int hour, minute;               /* local time, valid if time_valid */
    /* time settings, firmware */
    char ntp_server[64];
    char tz[64];
    bool clock_24h;
    char ota_url[192];
    char fw_version[32];
    char fw_partition[16];
    ui_ota_state_t ota_state;
    uint8_t ota_percent;
    char ota_message[64];
    char vm_number[32];             /* settings vm_number */
    bool dnd;                       /* settings dnd (the phone follows it a moment later) */
    /* sounds + call features (settings) */
    uint8_t ringtone_count;
    uint8_t ring_tone, ring_volume, call_volume, speaker_volume;
    bool call_waiting, auto_answer;
    /* quiet hours (settings) */
    bool quiet_hours, quiet_allow_contacts;
    uint16_t quiet_start, quiet_end;
    /* call forwarding (settings) */
    char fwd_always[UI_NUMBER_LEN], fwd_busy[UI_NUMBER_LEN], fwd_noanswer[UI_NUMBER_LEN];
    uint16_t fwd_noanswer_s;
    /* last CALL_EVENT_MISSED (call refused without ringing) */
    uint16_t missed_seq;            /* +1 per event */
    bool missed_dnd;
    bool missed_quiet;
    char missed_from[64];           /* name, else number */
    /* keypad lock when the display goes dark (ui_model_t keypad_autolock); default off.
     * Set it under b.mtx, e.g. in update_settings_locked() from a settings switch. */
    bool keypad_autolock;
} ui_bridge_local_t;

/** Fill the time/update settings of `loc` (Settings > Time and > Update prefill).
 *  NULL strings count as empty. */
void ui_bridge_fill_time_ota(ui_bridge_local_t *loc, const char *ntp_server, const char *tz,
                             bool clock_24h, const char *ota_url);

/** Firmware version and running partition label (set once at start). */
void ui_bridge_fill_fw(ui_bridge_local_t *loc, const char *version, const char *partition);

/** Current update progress from the ota component. */
void ui_bridge_fill_ota(ui_bridge_local_t *loc, const ota_status_t *st);

ui_ota_state_t ui_bridge_map_ota(ota_state_t st);

/** Fill the settings-derived fields of `loc` (account string, form prefill).
 *  The passwords are only looked at for "is one set"; they are never copied.
 *  NULL strings count as empty. */
void ui_bridge_fill_settings(ui_bridge_local_t *loc, const char *wifi_ssid, const char *wifi_pass,
                             const char *sip_server, uint16_t sip_port, const char *sip_user,
                             const char *sip_pass, const char *sip_display_name);

/** Voicemail number (settings `vm_number`); NULL counts as empty. */
void ui_bridge_fill_vm(ui_bridge_local_t *loc, const char *vm_number);

/** Do-not-disturb setting (read from settings so a tap shows at once, before the phone applies it). */
void ui_bridge_fill_dnd(ui_bridge_local_t *loc, bool dnd);
/** Ringtone list size + stored sounds / call feature settings (Settings > Sounds, Call features). */
void ui_bridge_fill_sounds(ui_bridge_local_t *loc, int ringtone_count, uint8_t ring_tone, uint8_t ring_volume,
                           uint8_t call_volume, uint8_t speaker_volume, bool call_waiting, bool auto_answer,
                           bool dnd);

/** Quiet hours settings (Settings > Call features prefill, status bar). */
void ui_bridge_fill_quiet(ui_bridge_local_t *loc, bool on, uint16_t start_min, uint16_t end_min,
                          bool allow_contacts);

/** Call forwarding settings (Settings > Forwarding prefill, status bar FWD). NULL = "". */
void ui_bridge_fill_forward(ui_bridge_local_t *loc, const char *always, const char *busy, const char *noanswer,
                            uint16_t noanswer_s);

ui_blf_state_t ui_bridge_map_blf(blf_state_t st);

ui_reg_state_t ui_bridge_map_reg(sip_reg_state_t st);
ui_call_state_t ui_bridge_map_call(call_state_t st);

/** Transfer progress (call features). */
ui_xfer_state_t ui_bridge_map_xfer(xfer_state_t st);

/** Background call: a call still CALL_ACTIVE there only awaits its hold confirmation, so it
 *  is shown as held; everything else maps like ui_bridge_map_call. */
ui_call_state_t ui_bridge_map_second(call_state_t st);

/** "Name <user@server>", "user@server", "user" or "" (no user). */
void ui_bridge_format_account(char *out, size_t size, const char *display_name, const char *user,
                              const char *server);

/** Whole seconds since the call was answered; 0 outside ACTIVE/HELD. */
uint32_t ui_bridge_call_seconds(call_state_t st, int64_t start_us, int64_t now_us);

/** Clock text for the status bar / home screen, or "" if the clock is not valid
 *  (or hour/minute are out of range).
 *  24 h: "HH:MM" ("03:07", "00:05"). 12 h: "H:MM AM|PM" without a leading zero
 *  ("3:07 PM"); midnight hour is 12 AM ("12:05 AM"), noon hour is 12 PM ("12:00 PM"). */
void ui_bridge_format_time(bool valid, int hour, int minute, bool clock_24h, char out[UI_TIME_STR_LEN]);

/** The whole model. BLF tiles and MWI come from the phone status (MWI counts
 *  are 0 while unknown), the voicemail number from `loc`. */
void ui_bridge_build_model(const ui_bridge_local_t *loc, const phone_status_t *ph, int64_t now_us,
                           ui_model_t *out);
