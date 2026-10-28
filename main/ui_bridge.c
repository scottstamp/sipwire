#include "ui_bridge.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "display.h"
#include "net.h"
#include "phone.h"
#include "sdkconfig.h"
#include "settings.h"
#include "sipwire_events.h"
#include "ui.h"
#include "ui_bridge_model.h"

static const char *TAG = "ui_bridge";

#define BRIDGE_STACK        6144        /* ui_update may build screens on this stack */
#define BRIDGE_PRIO         3           /* below the LVGL port task (4) and sip (5) */
#define BRIDGE_CORE         0
#define QUEUE_LEN           16
#define TICK_MS             1000
#define TIME_VALID_AFTER    1700000000  /* 2023-11: anything earlier = clock not set */

typedef enum {
    MSG_REFRESH,
    MSG_KEY,
} msg_kind_t;

typedef struct {
    msg_kind_t kind;
    char key;
    ui_key_action_t action;
} bridge_msg_t;

static struct {
    QueueHandle_t q;
    SemaphoreHandle_t mtx;
    bool sntp_started;              /* event loop task only */

    /* ---- under mtx ---- */
    ui_bridge_local_t loc;
    bool wifi_pending;
    char ssid[sizeof(((settings_t *)0)->wifi_ssid)];
    char wpass[sizeof(((settings_t *)0)->wifi_pass)];
    bool acc_pending;
    char server[sizeof(((settings_t *)0)->sip_server)];
    uint16_t port;
    char user[sizeof(((settings_t *)0)->sip_user)];
    char apass[sizeof(((settings_t *)0)->sip_password)];
    char dname[sizeof(((settings_t *)0)->sip_display_name)];
} b;

static void kick(void)
{
    const bridge_msg_t m = { .kind = MSG_REFRESH };
    xQueueSend(b.q, &m, 0);         /* full queue: a refresh is coming anyway */
}

static void update_account_locked(void)
{
    static settings_t st;           /* callers hold b.mtx */
    settings_get(&st);
    ui_bridge_format_account(b.loc.account, sizeof(b.loc.account), st.sip_display_name, st.sip_user,
                             st.sip_server);
}

/* ------------------------------------------------------------------ */
/* ui_actions_t (LVGL task, lock held): never block                   */
/* ------------------------------------------------------------------ */

static void act_dial(void *ctx, const char *number)
{
    (void)ctx;
    phone_dial(number);
}

static void act_answer(void *ctx)
{
    (void)ctx;
    phone_answer();
}

static void act_hangup(void *ctx)
{
    (void)ctx;
    phone_hangup();
}

static void act_dtmf(void *ctx, char digit)
{
    (void)ctx;
    phone_dtmf(digit);
}

static void act_set_mute(void *ctx, bool muted)
{
    (void)ctx;
    phone_set_mute(muted);
    kick();
}

static void act_set_speaker(void *ctx, bool on)
{
    (void)ctx;
    phone_set_speaker(on);
    kick();
}

static void act_set_volume(void *ctx, uint8_t volume)
{
    (void)ctx;
    phone_set_volume(volume);
    kick();
}

static void act_hold(void *ctx, bool hold)
{
    (void)ctx;
    phone_hold(hold);
}

static bool fits(const char *s, size_t size)
{
    return s && strlen(s) < size;
}

/* Persisting goes through NVS (slow): hand it to the bridge task. */
static void act_save_wifi(void *ctx, const char *ssid, const char *pass)
{
    (void)ctx;
    if (!fits(ssid, sizeof(b.ssid)) || !fits(pass ? pass : "", sizeof(b.wpass))) {
        ESP_LOGW(TAG, "wifi credentials too long");
        return;
    }
    xSemaphoreTake(b.mtx, portMAX_DELAY);
    strlcpy(b.ssid, ssid, sizeof(b.ssid));
    strlcpy(b.wpass, pass ? pass : "", sizeof(b.wpass));
    b.wifi_pending = true;
    xSemaphoreGive(b.mtx);
    kick();
}

/* An empty password keeps the stored one (the form never shows it). */
static void act_save_account(void *ctx, const char *server, uint16_t port, const char *user,
                             const char *password, const char *display_name)
{
    (void)ctx;
    if (!fits(server, sizeof(b.server)) || !fits(user, sizeof(b.user)) ||
        !fits(password ? password : "", sizeof(b.apass)) ||
        !fits(display_name ? display_name : "", sizeof(b.dname))) {
        ESP_LOGW(TAG, "account field too long");
        return;
    }
    xSemaphoreTake(b.mtx, portMAX_DELAY);
    strlcpy(b.server, server, sizeof(b.server));
    b.port = port ? port : 5060;
    strlcpy(b.user, user, sizeof(b.user));
    strlcpy(b.apass, password ? password : "", sizeof(b.apass));
    strlcpy(b.dname, display_name ? display_name : "", sizeof(b.dname));
    b.acc_pending = true;
    xSemaphoreGive(b.mtx);
    kick();
}

static const ui_actions_t s_actions = {
    .dial = act_dial,
    .answer = act_answer,
    .hangup = act_hangup,
    .dtmf = act_dtmf,
    .set_mute = act_set_mute,
    .set_speaker = act_set_speaker,
    .set_volume = act_set_volume,
    .hold = act_hold,
    .save_wifi = act_save_wifi,
    .save_account = act_save_account,
};

/* ------------------------------------------------------------------ */
/* event handlers (default loop task): update state, kick the task    */
/* ------------------------------------------------------------------ */

static void start_sntp(void)
{
    if (b.sntp_started) {
        return;
    }
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(CONFIG_SIPWIRE_NTP_SERVER);
    cfg.wait_for_sync = false;
    esp_err_t err = esp_netif_sntp_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "sntp: %s", esp_err_to_name(err));
        return;
    }
    b.sntp_started = true;
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == SIPWIRE_KEY_EVENT) {
        display_wake();
        bridge_msg_t m = { .kind = MSG_KEY };
        switch (id) {
        case KEY_EVENT_PRESS:  m.action = UI_KEY_PRESS; break;
        case KEY_EVENT_LONG:   m.action = UI_KEY_LONG; break;
        case KEY_EVENT_REPEAT: m.action = UI_KEY_REPEAT; break;
        case KEY_EVENT_RELEASE:
        default:               return;
        }
        if (data == NULL) {
            return;
        }
        m.key = ((const key_event_t *)data)->key;
        if (xQueueSend(b.q, &m, 0) != pdTRUE) {
            ESP_LOGW(TAG, "key '%c' dropped", m.key);
        }
        return;
    }
    if (base == SIPWIRE_CALL_EVENT && data &&
        ((const call_event_state_t *)data)->state == CALL_RINGING_IN) {
        display_wake();
    }

    xSemaphoreTake(b.mtx, portMAX_DELAY);
    if (base == SIPWIRE_NET_EVENT) {
        switch (id) {
        case NET_EVENT_CONNECTING:
            b.loc.net = UI_NET_CONNECTING;
            break;
        case NET_EVENT_GOT_IP:
            b.loc.net = UI_NET_CONNECTED;
            if (data) {
                strlcpy(b.loc.ip, ((const net_event_ip_t *)data)->ip_str, sizeof(b.loc.ip));
            }
            break;
        case NET_EVENT_DISCONNECTED:
            b.loc.net = UI_NET_OFF;
            b.loc.ip[0] = '\0';
            break;
        case NET_EVENT_RSSI:
            if (data) {
                b.loc.rssi = *(const int8_t *)data;
            }
            break;
        default:
            break;
        }
    } else if (base == SIPWIRE_SETTINGS_EVENT) {
        update_account_locked();
    }
    /* SIP / CALL events: the phone status already holds the news */
    xSemaphoreGive(b.mtx);

    if (base == SIPWIRE_NET_EVENT && id == NET_EVENT_GOT_IP) {
        start_sntp();
    }
    kick();
}

/* ------------------------------------------------------------------ */
/* bridge task                                                        */
/* ------------------------------------------------------------------ */

static void do_saves(void)
{
    static settings_t st;           /* bridge task only */
    bool wifi, acc;

    xSemaphoreTake(b.mtx, portMAX_DELAY);
    wifi = b.wifi_pending;
    acc = b.acc_pending;
    if (!wifi && !acc) {
        xSemaphoreGive(b.mtx);
        return;
    }
    settings_get(&st);
    if (wifi) {
        strlcpy(st.wifi_ssid, b.ssid, sizeof(st.wifi_ssid));
        strlcpy(st.wifi_pass, b.wpass, sizeof(st.wifi_pass));
    }
    if (acc) {
        strlcpy(st.sip_server, b.server, sizeof(st.sip_server));
        st.sip_port = b.port;
        strlcpy(st.sip_user, b.user, sizeof(st.sip_user));
        if (b.apass[0]) {
            strlcpy(st.sip_password, b.apass, sizeof(st.sip_password));
        }
        strlcpy(st.sip_display_name, b.dname, sizeof(st.sip_display_name));
    }
    b.wifi_pending = false;
    b.acc_pending = false;
    memset(b.wpass, 0, sizeof(b.wpass));
    memset(b.apass, 0, sizeof(b.apass));
    xSemaphoreGive(b.mtx);

    esp_err_t err = settings_set(&st);
    memset(st.wifi_pass, 0, sizeof(st.wifi_pass));
    memset(st.sip_password, 0, sizeof(st.sip_password));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "saving settings: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "settings saved from the UI (%s%s%s)", wifi ? "wifi" : "", wifi && acc ? ", " : "",
                 acc ? "account" : "");
    }
}

static void refresh(void)
{
    static ui_model_t last, model;  /* bridge task only */
    static bool have_last;
    ui_bridge_local_t loc;
    phone_status_t ph;

    xSemaphoreTake(b.mtx, portMAX_DELAY);
    loc = b.loc;
    xSemaphoreGive(b.mtx);

    time_t now = time(NULL);
    struct tm tm;
    loc.time_valid = now > TIME_VALID_AFTER && localtime_r(&now, &tm) != NULL;
    if (loc.time_valid) {
        loc.hour = tm.tm_hour;
        loc.minute = tm.tm_min;
    }

    phone_get_status(&ph);
    ui_bridge_build_model(&loc, &ph, esp_timer_get_time(), &model);
    if (have_last && memcmp(&model, &last, sizeof(model)) == 0) {
        return;
    }
    lvgl_port_lock(0);
    ui_update(&model);
    lvgl_port_unlock();
    last = model;
    have_last = true;
}

static void bridge_task(void *arg)
{
    (void)arg;
    int64_t next_tick = esp_timer_get_time() / 1000;
    for (;;) {
        int64_t now = esp_timer_get_time() / 1000;
        int64_t wait = next_tick - now;
        bridge_msg_t m;
        if (xQueueReceive(b.q, &m, wait > 0 ? pdMS_TO_TICKS(wait) : 0) == pdTRUE) {
            do {
                if (m.kind == MSG_KEY) {
                    lvgl_port_lock(0);
                    ui_key(m.key, m.action);
                    lvgl_port_unlock();
                }
            } while (xQueueReceive(b.q, &m, 0) == pdTRUE);
        }
        do_saves();
        refresh();
        now = esp_timer_get_time() / 1000;
        if (now >= next_tick) {
            next_tick = now + TICK_MS;
        }
    }
}

/* ------------------------------------------------------------------ */

esp_err_t ui_bridge_init(lv_display_t *disp)
{
    ESP_RETURN_ON_FALSE(b.q == NULL, ESP_ERR_INVALID_STATE, TAG, "already initialised");
    b.q = xQueueCreate(QUEUE_LEN, sizeof(bridge_msg_t));
    b.mtx = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(b.q && b.mtx, ESP_ERR_NO_MEM, TAG, "no memory");

    setenv("TZ", CONFIG_SIPWIRE_TZ, 1);
    tzset();

    xSemaphoreTake(b.mtx, portMAX_DELAY);
    b.loc.net = net_is_connected() ? UI_NET_CONNECTED : UI_NET_OFF;
    strlcpy(b.loc.ip, net_ip_str(), sizeof(b.loc.ip));
    update_account_locked();
    xSemaphoreGive(b.mtx);

    lvgl_port_lock(0);
    ui_init(disp, &s_actions);
    lvgl_port_unlock();

    static const esp_event_base_t *const bases[] = {
        &SIPWIRE_NET_EVENT, &SIPWIRE_SIP_EVENT, &SIPWIRE_CALL_EVENT, &SIPWIRE_KEY_EVENT, &SIPWIRE_SETTINGS_EVENT,
    };
    for (size_t i = 0; i < sizeof(bases) / sizeof(bases[0]); i++) {
        ESP_RETURN_ON_ERROR(esp_event_handler_register(*bases[i], ESP_EVENT_ANY_ID, on_event, NULL), TAG,
                            "register handler");
    }
    if (net_is_connected()) {
        start_sntp();
    }

    BaseType_t ok = xTaskCreatePinnedToCore(bridge_task, "ui_bridge", BRIDGE_STACK, NULL, BRIDGE_PRIO, NULL,
                                            BRIDGE_CORE);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "bridge task");
    kick();
    return ESP_OK;
}
