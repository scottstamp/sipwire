#pragma once

/* Over-the-air firmware update (esp_https_ota; plain http also works, see
 * CONFIG_ESP_HTTPS_OTA_ALLOW_HTTP in sdkconfig.defaults).
 *
 * ota_start() runs the update in its own task and reports through
 * SIPWIRE_OTA_EVENT (STARTED, PROGRESS, DONE, FAILED) and ota_get_status().
 * Before anything is activated the image is checked: the header must be for
 * this chip (esp32s3, enforced by esp_https_ota on the first block) and the
 * app descriptor's project_name must be "sipwire". On success the device
 * reboots after ~3 s into the other OTA slot.
 *
 * Rollback safety (CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE): a freshly updated
 * image boots as PENDING_VERIFY. ota_init() watches for "healthy" and then
 * calls esp_ota_mark_app_valid_cancel_rollback(). Healthy means, whichever
 * comes first:
 *   - the UI has started (ota_ui_started(), called by main) AND the network
 *     has an IP address (NET_EVENT_GOT_IP), or
 *   - 60 s after ota_init() without a reset.
 * A crash or reset before that makes the bootloader fall back to the previous
 * slot on the next boot. Without a pending update none of this does anything.
 *
 * Console: `ota [url]` (default: settings ota_url) and `ota status`. */

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef enum {
    OTA_STATE_IDLE,
    OTA_STATE_RUNNING,
    OTA_STATE_DONE,            /* image installed, reboot pending */
    OTA_STATE_FAILED,          /* `message` says why; a new ota_start() is allowed */
} ota_state_t;

typedef struct {
    ota_state_t state;
    uint8_t percent;           /* 0-100 while RUNNING (0 if the size is unknown), 100 when DONE */
    uint32_t bytes;            /* image bytes written */
    char message[64];          /* short status line for the UI / console */
} ota_status_t;

/** Longest URL ota_start() accepts (including the NUL); equals settings ota_url. */
#define OTA_URL_MAX 192

/** Register the console commands and start the rollback health watch.
 *  Call after net_init (and the default event loop). */
esp_err_t ota_init(void);

/** The UI is up: one of the two conditions that mark a pending image valid. */
void ota_ui_started(void);

/** Start an update from `url` (http:// or https://). Non-blocking.
 *  ESP_ERR_INVALID_STATE: an update is already running / about to reboot, or a call is not idle.
 *  ESP_ERR_INVALID_ARG: empty, too long or not an http(s) URL.
 *  A refusal other than "already running" also leaves state FAILED with the reason in `message`. */
esp_err_t ota_start(const char *url);

/** Snapshot of the current / last update; safe from any task. */
void ota_get_status(ota_status_t *out);

/** Label of the running app partition ("ota_0", "ota_1", "factory"). */
const char *ota_running_partition(void);
