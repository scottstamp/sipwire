#include "ota.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "console.h"
#include "net.h"
#include "phone.h"
#include "settings.h"
#include "sipwire_events.h"

static const char *TAG = "ota";

#define PROJECT_NAME        "sipwire"
#define HEALTHY_TIMEOUT_US  (60LL * 1000 * 1000)
#define REBOOT_DELAY_MS     3000
#define TASK_STACK          8192
#define TASK_PRIO           3
#define HTTP_TIMEOUT_MS     10000
#define POST_TIMEOUT        pdMS_TO_TICKS(100)

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static ota_status_t s_st;                   /* under s_mux */

/* ---- status / events ---- */

static void post(ota_event_id_t id, const void *data, size_t size)
{
    esp_err_t err = esp_event_post(SIPWIRE_OTA_EVENT, id, data, size, POST_TIMEOUT);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "post event %d failed: %s", (int)id, esp_err_to_name(err));
    }
}

static void set_status(ota_state_t state, uint8_t percent, uint32_t bytes, const char *msg)
{
    portENTER_CRITICAL(&s_mux);
    s_st.state = state;
    s_st.percent = percent;
    s_st.bytes = bytes;
    snprintf(s_st.message, sizeof(s_st.message), "%s", msg);
    portEXIT_CRITICAL(&s_mux);
}

static void fail(esp_err_t err, const char *reason)
{
    ESP_LOGE(TAG, "update failed: %s (%s)", reason, esp_err_to_name(err));
    set_status(OTA_STATE_FAILED, 0, 0, reason);
    ota_event_failed_t ev = { .err = err };
    snprintf(ev.reason, sizeof(ev.reason), "%s", reason);
    post(OTA_EVENT_FAILED, &ev, sizeof(ev));
}

void ota_get_status(ota_status_t *out)
{
    portENTER_CRITICAL(&s_mux);
    *out = s_st;
    portEXIT_CRITICAL(&s_mux);
}

const char *ota_running_partition(void)
{
    const esp_partition_t *p = esp_ota_get_running_partition();
    return p ? p->label : "?";
}

/* ---- rollback health ---- */

static esp_timer_handle_t s_health_timer;
static volatile bool s_pending;             /* running image still PENDING_VERIFY */
static volatile bool s_ui_up, s_net_up;

static void mark_valid(const char *why)
{
    if (!s_pending) {
        return;
    }
    s_pending = false;
    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "new firmware marked valid (%s)", why);
    } else {
        ESP_LOGE(TAG, "mark valid failed: %s", esp_err_to_name(err));
    }
    if (s_health_timer) {
        esp_timer_stop(s_health_timer);
    }
}

static void check_health(void)
{
    if (s_ui_up && s_net_up) {
        mark_valid("UI started and network up");
    }
}

static void health_timeout(void *arg)
{
    (void)arg;
    mark_valid("60 s since boot");
}

static void on_net_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)id;
    (void)data;
    s_net_up = true;
    check_health();
}

void ota_ui_started(void)
{
    s_ui_up = true;
    check_health();
}

/* ---- the update task ---- */

static const char *describe_err(esp_err_t err)
{
    switch (err) {
    case ESP_ERR_INVALID_VERSION:       return "Image is for another chip";
    case ESP_ERR_OTA_VALIDATE_FAILED:   return "Image is corrupt";
    case ESP_ERR_OTA_SPI_MODE_MISMATCH: return "Image flash mode mismatch";
    case ESP_ERR_HTTP_CONNECT:          return "Cannot connect to server";
    case ESP_ERR_NO_MEM:                return "Out of memory";
    default:                            return "Download failed";
    }
}

/* Each 4 KB sector erase ahead of a write parks CPU 0 (~45 ms) and Wi-Fi traffic arrives about
 * as fast, so during the download IDLE0 may not run for the 5 s task watchdog period. That is
 * expected (and harmless): stop watching IDLE0 for the download, watch it again afterwards. */
static void idle0_watch(bool on)
{
#if CONFIG_ESP_TASK_WDT_INIT
    uint32_t mask = 0;
#if CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0
    mask |= on ? BIT(0) : 0;
#endif
#if CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1
    mask |= BIT(1);
#endif
    const esp_task_wdt_config_t cfg = {
        .timeout_ms = CONFIG_ESP_TASK_WDT_TIMEOUT_S * 1000,
        .idle_core_mask = mask,
#if CONFIG_ESP_TASK_WDT_PANIC
        .trigger_panic = true,
#endif
    };
    esp_err_t err = esp_task_wdt_reconfigure(&cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "task watchdog reconfigure: %s", esp_err_to_name(err));
    }
#else
    (void)on;
#endif
}

static void ota_task(void *arg)
{
    char *url = arg;
    esp_https_ota_handle_t h = NULL;
    esp_err_t err;
    char msg[sizeof(s_st.message)];

    ESP_LOGI(TAG, "starting update from %s", url);
    post(OTA_EVENT_STARTED, NULL, 0);

    esp_http_client_config_t http = {
        .url = url,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .keep_alive_enable = true,
    };
    if (strncmp(url, "https://", 8) == 0) {
        http.crt_bundle_attach = esp_crt_bundle_attach;
    }
    esp_https_ota_config_t cfg = { .http_config = &http };

    err = esp_https_ota_begin(&cfg, &h);
    if (err != ESP_OK) {
        fail(err, describe_err(err));
        goto out;
    }

    /* The image header is in: check what this is before any flash is written. */
    esp_app_desc_t nd;
    err = esp_https_ota_get_img_desc(h, &nd);
    if (err != ESP_OK) {
        fail(err, "Not a firmware image");
        goto abort;
    }
    if (strncmp(nd.project_name, PROJECT_NAME, sizeof(nd.project_name)) != 0) {
        snprintf(msg, sizeof(msg), "Wrong firmware: %.24s", nd.project_name);
        fail(ESP_ERR_INVALID_VERSION, msg);
        goto abort;
    }
    ESP_LOGI(TAG, "version %s -> %s", esp_app_get_description()->version, nd.version);

    set_status(OTA_STATE_RUNNING, 0, 0, "Downloading 0%");
    int last_pct = -1, last_log = -1;
    int size = esp_https_ota_get_image_size(h);
    idle0_watch(false);
    while ((err = esp_https_ota_perform(h)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
        int read = esp_https_ota_get_image_len_read(h);
        int pct = size > 0 ? (int)((int64_t)read * 100 / size) : 0;
        if (pct != last_pct) {
            last_pct = pct;
            snprintf(msg, sizeof(msg), "Downloading %d%%", pct);
            set_status(OTA_STATE_RUNNING, (uint8_t)pct, (uint32_t)read, msg);
            ota_event_progress_t ev = { .percent = (uint8_t)pct, .bytes = (uint32_t)read };
            post(OTA_EVENT_PROGRESS, &ev, sizeof(ev));
            if (pct / 10 != last_log) {
                last_log = pct / 10;
                ESP_LOGI(TAG, "progress %d%% (%d of %d bytes), internal free %u, min %u", pct, read, size,
                         (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                         (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
            }
        }
    }
    idle0_watch(true);
    if (err != ESP_OK) {
        fail(err, describe_err(err));
        goto abort;
    }
    if (!esp_https_ota_is_complete_data_received(h)) {
        fail(ESP_FAIL, "Download incomplete");
        goto abort;
    }

    set_status(OTA_STATE_RUNNING, 100, (uint32_t)esp_https_ota_get_image_len_read(h), "Verifying...");
    err = esp_https_ota_finish(h);     /* checks the image hash, switches the boot slot */
    h = NULL;
    if (err != ESP_OK) {
        fail(err, describe_err(err));
        goto out;
    }

    ESP_LOGI(TAG, "update done, rebooting in %d s", REBOOT_DELAY_MS / 1000);
    set_status(OTA_STATE_DONE, 100, (uint32_t)size, "Rebooting in 3 s...");
    post(OTA_EVENT_DONE, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(REBOOT_DELAY_MS));
    esp_restart();

abort:
    esp_https_ota_abort(h);
out:
    free(url);
    vTaskDelete(NULL);
}

/* ---- public API ---- */

esp_err_t ota_start(const char *url)
{
    if (url == NULL || strlen(url) == 0 || strlen(url) >= OTA_URL_MAX ||
        (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0)) {
        fail(ESP_ERR_INVALID_ARG, "URL must start with http://");
        return ESP_ERR_INVALID_ARG;
    }

    phone_status_t ph;
    phone_get_status(&ph);

    /* Claim the updater first so two starts cannot race. */
    bool busy = false;
    portENTER_CRITICAL(&s_mux);
    if (s_st.state == OTA_STATE_RUNNING || s_st.state == OTA_STATE_DONE) {
        busy = true;
    } else if (ph.call == CALL_IDLE) {
        s_st = (ota_status_t){ .state = OTA_STATE_RUNNING };
        snprintf(s_st.message, sizeof(s_st.message), "Connecting...");
    }
    portEXIT_CRITICAL(&s_mux);
    if (busy) {
        ESP_LOGW(TAG, "an update is already running");
        return ESP_ERR_INVALID_STATE;
    }
    if (ph.call != CALL_IDLE) {
        fail(ESP_ERR_INVALID_STATE, "Not during a call");
        return ESP_ERR_INVALID_STATE;
    }

    char *copy = strdup(url);
    if (copy == NULL || xTaskCreate(ota_task, "ota", TASK_STACK, copy, TASK_PRIO, NULL) != pdPASS) {
        free(copy);
        fail(ESP_ERR_NO_MEM, "Out of memory");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/* ---- console ---- */

static const char *app_state_name(esp_ota_img_states_t st)
{
    switch (st) {
    case ESP_OTA_IMG_NEW:            return "new";
    case ESP_OTA_IMG_PENDING_VERIFY: return "pending-verify";
    case ESP_OTA_IMG_VALID:          return "valid";
    case ESP_OTA_IMG_INVALID:        return "invalid";
    case ESP_OTA_IMG_ABORTED:        return "aborted";
    default:                         return "undefined";
    }
}

static const char *update_state_name(ota_state_t s)
{
    switch (s) {
    case OTA_STATE_RUNNING: return "running";
    case OTA_STATE_DONE:    return "done";
    case OTA_STATE_FAILED:  return "failed";
    default:                return "idle";
    }
}

static int cmd_ota(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "status") == 0) {
        const esp_partition_t *run = esp_ota_get_running_partition();
        const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
        esp_ota_img_states_t st;
        const char *state = esp_ota_get_state_partition(run, &st) == ESP_OK ? app_state_name(st) : "n/a";
        printf("running partition: %s (0x%" PRIx32 ")\n", run ? run->label : "?", run ? run->address : 0);
        printf("version: %s\n", esp_app_get_description()->version);
        printf("app state: %s\n", state);
        printf("next slot: %s\n", next ? next->label : "?");
        ota_status_t os;
        ota_get_status(&os);
        printf("update: %s %u%% %s\n", update_state_name(os.state), os.percent, os.message);
        return 0;
    }
    if (argc > 2) {
        printf("usage: ota [url] | ota status\n");
        return 1;
    }
    char ota_url[SETTINGS_FIELD_SIZE(ota_url)];
    SETTINGS_GET(ota_url, &ota_url);
    const char *url = argc == 2 ? argv[1] : ota_url;
    if (url[0] == '\0') {
        printf("no URL: `ota <url>` or `cfg set ota_url <url>`\n");
        return 1;
    }
    esp_err_t err = ota_start(url);
    if (err != ESP_OK) {
        ota_status_t os;
        ota_get_status(&os);
        printf("not started: %s\n", os.state == OTA_STATE_FAILED ? os.message : esp_err_to_name(err));
        return 1;
    }
    printf("update started from %s\n", url);
    return 0;
}

esp_err_t ota_init(void)
{
    static const esp_console_cmd_t cmd = {
        .command = "ota", .help = "Firmware update from a URL (default: cfg ota_url), or show status",
        .hint = "[url] | status", .func = cmd_ota,
    };
    ESP_RETURN_ON_ERROR(console_register_command(&cmd), TAG, "register ota");

    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (run && esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY) {
        s_pending = true;
        ESP_LOGW(TAG, "new firmware on %s is pending verification: valid after UI + network, or in 60 s",
                 run->label);
        ESP_RETURN_ON_ERROR(esp_event_handler_register(SIPWIRE_NET_EVENT, NET_EVENT_GOT_IP, on_net_event, NULL),
                            TAG, "register net handler");
        const esp_timer_create_args_t ta = { .callback = health_timeout, .name = "ota_health" };
        ESP_RETURN_ON_ERROR(esp_timer_create(&ta, &s_health_timer), TAG, "health timer");
        ESP_RETURN_ON_ERROR(esp_timer_start_once(s_health_timer, HEALTHY_TIMEOUT_US), TAG, "health timer");
        if (net_is_connected()) {
            s_net_up = true;
        }
    }
    return ESP_OK;
}
