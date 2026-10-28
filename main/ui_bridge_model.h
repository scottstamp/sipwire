#pragma once

/* Pure part of the UI bridge: builds the ui_model_t snapshot from the
 * phone status and the bridge's own net/clock/account state. No FreeRTOS or
 * LVGL calls, host-tested in test_host/phone. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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
} ui_bridge_local_t;

/** Fill the settings-derived fields of `loc` (account string, form prefill).
 *  The passwords are only looked at for "is one set"; they are never copied.
 *  NULL strings count as empty. */
void ui_bridge_fill_settings(ui_bridge_local_t *loc, const char *wifi_ssid, const char *wifi_pass,
                             const char *sip_server, uint16_t sip_port, const char *sip_user,
                             const char *sip_pass, const char *sip_display_name);

ui_reg_state_t ui_bridge_map_reg(sip_reg_state_t st);
ui_call_state_t ui_bridge_map_call(call_state_t st);

/** "Name <user@server>", "user@server", "user" or "" (no user). */
void ui_bridge_format_account(char *out, size_t size, const char *display_name, const char *user,
                              const char *server);

/** Whole seconds since the call was answered; 0 outside ACTIVE/HELD. */
uint32_t ui_bridge_call_seconds(call_state_t st, int64_t start_us, int64_t now_us);

/** "HH:MM", or "" if the clock is not valid. */
void ui_bridge_format_hhmm(bool valid, int hour, int minute, char out[6]);

void ui_bridge_build_model(const ui_bridge_local_t *loc, const phone_status_t *ph, int64_t now_us,
                           ui_model_t *out);
