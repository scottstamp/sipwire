#pragma once

/* Device-side glue between the phone/net/settings and the LVGL UI.
 *
 * - builds ui_model_t from the phone status + net events + settings and
 *   pushes it with ui_update() (on every relevant event and once a second
 *   for the call timer and the clock);
 * - implements ui_actions_t (dial, answer, ... → phone_*, save_wifi /
 *   save_account / save_time → settings_set, ota_start → ota_start());
 * - forwards SIPWIRE_KEY_EVENT to ui_key();
 * - starts SNTP on the first GOT_IP and applies the stored TZ / NTP server
 *   (settings `tz`, `ntp_server`; defaults CONFIG_SIPWIRE_TZ / _NTP_SERVER);
 *   a change saved from the UI or `cfg set` takes effect live: setenv("TZ") +
 *   tzset(), SNTP restarted with the new server, no reboot;
 * - mirrors the ota component's progress into the model.
 *
 * All LVGL calls happen in the "ui_bridge" task under lvgl_port_lock(), so
 * event handlers and UI actions never block on the LVGL lock. The ENDED →
 * IDLE delay is owned by the phone component; the bridge only mirrors it. */

#include "esp_err.h"
#include "lvgl.h"

/** Build the UI on `disp` (ui_init) and start the bridge. Call after
 *  settings_init/net_init and before phone_init so no event is missed. */
esp_err_t ui_bridge_init(lv_display_t *disp);
