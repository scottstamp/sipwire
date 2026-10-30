#include "ui_bridge.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "audio.h"
#include "directory.h"
#include "display.h"
#include "net.h"
#include "ota.h"
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
    SemaphoreHandle_t time_mtx;     /* guards sntp_started and the applied TZ / NTP server */
    bool sntp_started;

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
    bool cal_pending;
    ui_touch_cal_t cal;
    bool time_pending;
    char ntp[sizeof(((settings_t *)0)->ntp_server)];
    char tz[sizeof(((settings_t *)0)->tz)];
    bool clock_24h;
    bool ota_pending;
    char ota_url[sizeof(((settings_t *)0)->ota_url)];
    bool blf_pending;
    char blf[sizeof(((settings_t *)0)->blf)];
    bool vm_pending;
    bool dnd_pending;
    bool dnd_value;
    char vm_number[sizeof(((settings_t *)0)->vm_number)];
    bool snd_pending;               /* Settings > Sounds */
    uint8_t snd_tone, snd_ring, snd_call, snd_spk;
    bool feat_pending;              /* Settings > Call features */
    bool feat_cw, feat_aa, feat_dnd;
    bool quiet_pending;             /* Settings > Call features: quiet hours */
    bool quiet_on, quiet_allow;
    uint16_t quiet_start, quiet_end;
} b;

/* Scratch the bridge needs now and then (~6 KB): in PSRAM when there is some, as internal RAM is
 * the scarce resource. Nothing here is touched per audio or LVGL frame (refresh runs once a
 * second and on events). */
static struct {
    settings_t fill;                /* update_settings_locked(): callers hold b.mtx */
    settings_t save;                /* do_saves(): bridge task only */
    ui_model_t last, model;         /* refresh(): bridge task only */
} *s_cold;

/* What SNTP and the C library are using right now. The NTP buffer must stay valid
 * for as long as SNTP runs: lwIP keeps the server name pointer. */
static char s_ntp[sizeof(((settings_t *)0)->ntp_server)];
static char s_tz[sizeof(((settings_t *)0)->tz)];

static void kick(void)
{
    const bridge_msg_t m = { .kind = MSG_REFRESH };
    xQueueSend(b.q, &m, 0);         /* full queue: a refresh is coming anyway */
}

/* Account string + the non-secret fields that prefill the Settings forms. */
static void update_settings_locked(void)
{
    settings_t *st = &s_cold->fill;
    settings_get(st);
    ui_bridge_fill_settings(&b.loc, st->wifi_ssid, st->wifi_pass, st->sip_server, st->sip_port, st->sip_user,
                            st->sip_password, st->sip_display_name);
    ui_bridge_fill_time_ota(&b.loc, st->ntp_server, st->tz, st->clock_24h, st->ota_url);
    ui_bridge_fill_vm(&b.loc, st->vm_number);
    ui_bridge_fill_dnd(&b.loc, st->dnd);
    ui_bridge_fill_sounds(&b.loc, audio_ringtone_count(), st->ring_tone, st->ring_volume, st->call_volume,
                          st->speaker_volume, st->call_waiting, st->auto_answer, st->dnd);
    ui_bridge_fill_quiet(&b.loc, st->quiet_hours, st->quiet_start, st->quiet_end, st->quiet_allow_contacts);
    memset(st->wifi_pass, 0, sizeof(st->wifi_pass));
    memset(st->sip_password, 0, sizeof(st->sip_password));
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

/* Persisting goes through NVS (slow): hand it to the bridge task.
 * An empty password keeps the stored one (the form never shows it). */
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

/* Likewise: an empty password keeps the stored one. */
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

/* Display hook (LVGL task, lock held): raw samples straight into the UI. */
static void raw_touch_cb(int32_t ax, int32_t ay, bool pressed, void *ctx)
{
    (void)ctx;
    ui_touch_raw(ax, ay, pressed);
}

static void act_touch_cal_begin(void *ctx)
{
    (void)ctx;
    display_touch_raw_hook(raw_touch_cb, NULL);
}

/* The hook goes off right here; the NVS write is left to the bridge task and
 * the display picks the result up from the settings event. */
static void act_touch_cal_end(void *ctx, const ui_touch_cal_t *cal)
{
    (void)ctx;
    display_touch_raw_hook(NULL, NULL);
    if (!cal) {
        return;
    }
    xSemaphoreTake(b.mtx, portMAX_DELAY);
    b.cal = *cal;
    b.cal_pending = true;
    xSemaphoreGive(b.mtx);
    kick();
}

/* Time settings and the update URL go through NVS and the network: the bridge task does them. */
static void act_save_time(void *ctx, const char *ntp, const char *tz, bool clock_24h)
{
    (void)ctx;
    if (!ntp || !ntp[0] || !tz || !tz[0] || !fits(ntp, sizeof(b.ntp)) || !fits(tz, sizeof(b.tz))) {
        ESP_LOGW(TAG, "time settings empty or too long");
        return;
    }
    xSemaphoreTake(b.mtx, portMAX_DELAY);
    strlcpy(b.ntp, ntp, sizeof(b.ntp));
    strlcpy(b.tz, tz, sizeof(b.tz));
    b.clock_24h = clock_24h;
    b.time_pending = true;
    xSemaphoreGive(b.mtx);
    kick();
}

static void act_ota_start(void *ctx, const char *url)
{
    (void)ctx;
    if (!url || !url[0] || !fits(url, sizeof(b.ota_url))) {
        ESP_LOGW(TAG, "update URL empty or too long");
        return;
    }
    xSemaphoreTake(b.mtx, portMAX_DELAY);
    strlcpy(b.ota_url, url, sizeof(b.ota_url));
    b.ota_pending = true;
    xSemaphoreGive(b.mtx);
    kick();
}

/* Settings > BLF keys: validated here, stored (and applied live) by the bridge task. */
static void act_save_blf(void *ctx, const char *list)
{
    (void)ctx;
    if (!list || !fits(list, sizeof(b.blf)) || sipwire_blf_parse(list, NULL, SIPWIRE_BLF_MAX) < 0) {
        ESP_LOGW(TAG, "blf list invalid or too long");
        return;
    }
    xSemaphoreTake(b.mtx, portMAX_DELAY);
    strlcpy(b.blf, list, sizeof(b.blf));
    b.blf_pending = true;
    xSemaphoreGive(b.mtx);
    kick();
}

static void act_save_vm_number(void *ctx, const char *number)
{
    (void)ctx;
    if (!number || !fits(number, sizeof(b.vm_number))) {
        ESP_LOGW(TAG, "voicemail number too long");
        return;
    }
    xSemaphoreTake(b.mtx, portMAX_DELAY);
    strlcpy(b.vm_number, number, sizeof(b.vm_number));
    b.vm_pending = true;
    xSemaphoreGive(b.mtx);
    kick();
}

/* call features: all of these are queued commands in the phone, non-blocking */
static void act_answer_waiting(void *ctx)
{
    (void)ctx;
    phone_answer_waiting();
}

static void act_reject_waiting(void *ctx)
{
    (void)ctx;
    phone_reject_waiting();
}

static void act_swap(void *ctx)
{
    (void)ctx;
    phone_swap();
}

static void act_transfer_blind(void *ctx, const char *target)
{
    (void)ctx;
    phone_transfer_blind(target);
}

static void act_transfer_attended_start(void *ctx, const char *target)
{
    (void)ctx;
    phone_transfer_attended_start(target);
}

static void act_transfer_attended_complete(void *ctx)
{
    (void)ctx;
    phone_transfer_attended_complete();
}

static void act_transfer_attended_cancel(void *ctx)
{
    (void)ctx;
    phone_transfer_attended_cancel();
}

static void act_set_dnd(void *ctx, bool on)
{
    (void)ctx;
    /* persists setting `dnd` (NVS: not on the LVGL task); the bridge task applies it */
    xSemaphoreTake(b.mtx, portMAX_DELAY);
    b.dnd_value = on;
    b.dnd_pending = true;
    xSemaphoreGive(b.mtx);
    kick();
}

/* ---- directory: the lists are read straight from the directory (in RAM, one mutex); saves
 * change RAM at once (the screen refetches right after them) and leave the flash write to the
 * directory's writer task. ---- */

static size_t act_contacts_search(void *ctx, const char *query, ui_contact_t *out, size_t max)
{
    (void)ctx;
    if (max > DIR_CONTACTS_MAX) {
        max = DIR_CONTACTS_MAX;
    }
    /* up to 12.8 KB, only while the screen refetches: PSRAM (under the 16 KB
     * SPIRAM_MALLOC_ALWAYSINTERNAL threshold plain malloc would take internal RAM) */
    dir_contact_t *tmp = heap_caps_malloc_prefer(max * sizeof(*tmp), 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                                 MALLOC_CAP_8BIT);
    if (!tmp) {
        return 0;
    }
    char speed[UI_SPEED_LAST + 1][DIR_NUMBER_MAX] = {{0}};
    for (int d = UI_SPEED_FIRST; d <= UI_SPEED_LAST; d++) {
        if (dir_speed_get(d, speed[d], sizeof(speed[d])) != ESP_OK) {
            speed[d][0] = '\0';
        }
    }
    size_t n = dir_contact_search(query, tmp, max);
    for (size_t i = 0; i < n; i++) {
        strlcpy(out[i].name, tmp[i].name, sizeof(out[i].name));
        strlcpy(out[i].number, tmp[i].number, sizeof(out[i].number));
        out[i].speed = 0;
        for (int d = UI_SPEED_FIRST; d <= UI_SPEED_LAST; d++) {
            if (speed[d][0] && strcmp(speed[d], tmp[i].number) == 0) {
                out[i].speed = (uint8_t)d;
                break;
            }
        }
    }
    free(tmp);
    return n;
}

static const char *act_contact_save(void *ctx, const char *old_number, const char *name, const char *number,
                                    uint8_t speed)
{
    (void)ctx;
    char nn[DIR_NUMBER_MAX], on[DIR_NUMBER_MAX] = "";
    dir_normalize(number ? number : "", nn, sizeof(nn));
    if (!nn[0]) {
        return "Invalid number";
    }
    if (!name || !name[0]) {
        return "Enter a name";
    }
    if (old_number) {
        dir_normalize(old_number, on, sizeof(on));
    }
    esp_err_t err = dir_contact_set(nn, name);           /* first: a full list loses nothing */
    if (err != ESP_OK) {
        return err == ESP_ERR_NO_MEM ? "Contact list is full" : "Could not save";
    }
    if (on[0] && strcmp(on, nn) != 0) {
        dir_contact_del(on);
    }
    for (int d = UI_SPEED_FIRST; d <= UI_SPEED_LAST; d++) {   /* slots of this contact move or clear */
        char cur[DIR_NUMBER_MAX];
        if (dir_speed_get(d, cur, sizeof(cur)) == ESP_OK && (strcmp(cur, nn) == 0 || (on[0] && strcmp(cur, on) == 0))) {
            dir_speed_set(d, NULL);
        }
    }
    if (speed >= UI_SPEED_FIRST && speed <= UI_SPEED_LAST) {
        err = dir_speed_set(speed, nn);
        if (err != ESP_OK) {
            return "Could not set the speed dial key";
        }
    }
    return NULL;
}

static void act_contact_delete(void *ctx, const char *number)
{
    (void)ctx;
    char nn[DIR_NUMBER_MAX];
    dir_normalize(number, nn, sizeof(nn));
    dir_contact_del(nn);
    for (int d = UI_SPEED_FIRST; d <= UI_SPEED_LAST; d++) {
        char cur[DIR_NUMBER_MAX];
        if (dir_speed_get(d, cur, sizeof(cur)) == ESP_OK && strcmp(cur, nn) == 0) {
            dir_speed_set(d, NULL);
        }
    }
}

static bool act_speed_get(void *ctx, int digit, ui_contact_t *out)
{
    (void)ctx;
    memset(out, 0, sizeof(*out));
    if (dir_speed_get(digit, out->number, sizeof(out->number)) != ESP_OK) {
        return false;
    }
    dir_contact_lookup(out->number, out->name, sizeof(out->name));
    out->speed = (uint8_t)digit;
    return true;
}

static size_t act_history_list(void *ctx, ui_call_t *out, size_t max)
{
    (void)ctx;
    dir_call_t chunk[16];                       /* 16 x ~80 B on the stack, paged */
    size_t n = 0;
    while (n < max) {
        size_t want = max - n < 16 ? max - n : 16;
        size_t got = dir_history_list(chunk, n, want);
        for (size_t i = 0; i < got; i++) {
            ui_call_t *o = &out[n + i];
            o->direction = chunk[i].direction == DIR_CALL_OUT ? UI_CALLDIR_OUT : UI_CALLDIR_IN;
            o->result = chunk[i].result <= DIR_RES_DND ? chunk[i].result : UI_CALLRES_MISSED;
            o->seen = chunk[i].seen;
            o->start = (int64_t)chunk[i].start;
            o->duration_s = chunk[i].duration_s;
            strlcpy(o->number, chunk[i].number, sizeof(o->number));
            strlcpy(o->name, chunk[i].name, sizeof(o->name));
        }
        n += got;
        if (got < want) {
            break;
        }
    }
    return n;
}

static void act_history_clear(void *ctx)
{
    (void)ctx;
    dir_history_clear();
    kick();
}

static void act_history_mark_seen(void *ctx)
{
    (void)ctx;
    dir_history_mark_seen();
    kick();                         /* the home badge follows */
}

static bool act_last_dialed(void *ctx, char *out, size_t cap)
{
    (void)ctx;
    return dir_history_last_dialed(out, cap);
}

/* ---- sounds / call features ---- */

static const char *act_ringtone_name(void *ctx, int idx)
{
    (void)ctx;
    return audio_ringtone_name(idx);
}

/* The phone owns the speaker while a call exists: no preview then, and a stop must not cut a
 * ring that started after the preview (an incoming call pops over Settings). */
static void act_ringtone_preview(void *ctx, int idx)
{
    (void)ctx;
    phone_status_t ph;
    phone_get_status(&ph);
    if (ph.call != CALL_IDLE || ph.second.call != CALL_IDLE) {
        return;
    }
    if (idx < 0) {
        audio_stop_ringtone();
    } else {
        audio_play_ringtone(idx);
    }
}

static void act_save_sounds(void *ctx, uint8_t tone, uint8_t ring, uint8_t call, uint8_t spk)
{
    (void)ctx;
    xSemaphoreTake(b.mtx, portMAX_DELAY);
    b.snd_tone = tone;
    b.snd_ring = ring > 100 ? 100 : ring;
    b.snd_call = call > 100 ? 100 : call;
    b.snd_spk = spk > 100 ? 100 : spk;
    b.snd_pending = true;
    xSemaphoreGive(b.mtx);
    kick();
}

static void act_save_call_features(void *ctx, bool cw, bool aa, bool dnd)
{
    (void)ctx;
    xSemaphoreTake(b.mtx, portMAX_DELAY);
    b.feat_cw = cw;
    b.feat_aa = aa;
    b.feat_dnd = dnd;
    b.feat_pending = true;
    xSemaphoreGive(b.mtx);
    kick();
}

static void act_save_quiet(void *ctx, bool on, uint16_t start, uint16_t end, bool allow)
{
    (void)ctx;
    xSemaphoreTake(b.mtx, portMAX_DELAY);
    b.quiet_on = on;
    b.quiet_start = start;
    b.quiet_end = end;
    b.quiet_allow = allow;
    b.quiet_pending = true;
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
    .touch_cal_begin = act_touch_cal_begin,
    .touch_cal_end = act_touch_cal_end,
    .save_time = act_save_time,
    .ota_start = act_ota_start,
    .save_blf = act_save_blf,
    .save_vm_number = act_save_vm_number,
    .answer_waiting = act_answer_waiting,
    .reject_waiting = act_reject_waiting,
    .swap = act_swap,
    .transfer_blind = act_transfer_blind,
    .transfer_attended_start = act_transfer_attended_start,
    .transfer_attended_complete = act_transfer_attended_complete,
    .transfer_attended_cancel = act_transfer_attended_cancel,
    .set_dnd = act_set_dnd,
    .contacts_search = act_contacts_search,
    .contact_save = act_contact_save,
    .contact_delete = act_contact_delete,
    .speed_get = act_speed_get,
    .history_list = act_history_list,
    .history_clear = act_history_clear,
    .history_mark_seen = act_history_mark_seen,
    .last_dialed = act_last_dialed,
    .ringtone_name = act_ringtone_name,
    .ringtone_preview = act_ringtone_preview,
    .save_sounds = act_save_sounds,
    .save_call_features = act_save_call_features,
    .save_quiet = act_save_quiet,
};

/* ------------------------------------------------------------------ */
/* event handlers (default loop task): update state, kick the task    */
/* ------------------------------------------------------------------ */

/* Caller holds b.time_mtx. */
static void sntp_init_locked(void)
{
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(s_ntp);
    cfg.wait_for_sync = false;
    esp_err_t err = esp_netif_sntp_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "sntp: %s", esp_err_to_name(err));
        return;
    }
    b.sntp_started = true;
}

static void start_sntp(void)
{
    xSemaphoreTake(b.time_mtx, portMAX_DELAY);
    if (!b.sntp_started) {
        sntp_init_locked();
    }
    xSemaphoreGive(b.time_mtx);
}

/* Make the C library and SNTP follow the stored TZ / NTP server, live: no reboot.
 * The next refresh formats the clock in the new zone; SNTP re-queries the new server. */
static void apply_time_cfg(const char *ntp, const char *tz)
{
    xSemaphoreTake(b.time_mtx, portMAX_DELAY);
    if (strcmp(tz, s_tz) != 0) {
        strlcpy(s_tz, tz, sizeof(s_tz));
        setenv("TZ", s_tz, 1);
        tzset();
        ESP_LOGI(TAG, "time zone: %s", s_tz);
    }
    if (strcmp(ntp, s_ntp) != 0) {
        if (b.sntp_started) {
            esp_netif_sntp_deinit();
            b.sntp_started = false;
        }
        strlcpy(s_ntp, ntp, sizeof(s_ntp));
        ESP_LOGI(TAG, "ntp server: %s", s_ntp);
        if (net_is_connected()) {
            sntp_init_locked();
        }
    }
    xSemaphoreGive(b.time_mtx);
}

static void apply_time_from_settings(void)
{
    char ntp[sizeof(s_ntp)], tz[sizeof(s_tz)];
    SETTINGS_GET(ntp_server, &ntp);
    SETTINGS_GET(tz, &tz);
    apply_time_cfg(ntp, tz);
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == SIPWIRE_DISPLAY_EVENT) {
        /* backlight back on: one refresh now (clock, anything deferred while dark); the 1 s
         * ticks resume with it */
        if (id == DISPLAY_EVENT_BACKLIGHT && data && *(const bool *)data) {
            kick();
        }
        return;
    }
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
    if (base == SIPWIRE_CALL_EVENT && data && (id == CALL_EVENT_STATE || id == CALL_EVENT_SECOND) &&
        ((const call_event_state_t *)data)->state == CALL_RINGING_IN) {
        display_wake();
    }
    char missed_from[sizeof(b.loc.missed_from)] = "";
    if (base == SIPWIRE_CALL_EVENT && id == CALL_EVENT_MISSED && data) {
        /* a call refused without ringing: the UI shows who it was (the history has it already) */
        const call_event_missed_t *ev = data;
        strlcpy(missed_from, ev->name, sizeof(missed_from));
        if (!missed_from[0] && !dir_contact_lookup(ev->number, missed_from, sizeof(missed_from))) {
            dir_normalize(ev->number, missed_from, sizeof(missed_from));
        }
        if (!missed_from[0]) {
            strlcpy(missed_from, "Unknown", sizeof(missed_from));
        }
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
        update_settings_locked();
    } else if (base == SIPWIRE_CALL_EVENT && id == CALL_EVENT_MISSED && data) {
        b.loc.missed_seq++;
        b.loc.missed_dnd = ((const call_event_missed_t *)data)->dnd;
        b.loc.missed_quiet = ((const call_event_missed_t *)data)->quiet;
        strlcpy(b.loc.missed_from, missed_from, sizeof(b.loc.missed_from));
    }
    /* SIP / CALL / BLF events: the phone status already holds the news */
    xSemaphoreGive(b.mtx);

    if (base == SIPWIRE_NET_EVENT && id == NET_EVENT_GOT_IP) {
        start_sntp();
    }
    if (base == SIPWIRE_SETTINGS_EVENT) {
        apply_time_from_settings();
    }
    if (base == SIPWIRE_NET_EVENT && id == NET_EVENT_RSSI && !display_is_lit()) {
        return;     /* signal bars nobody sees: b.loc has them for the refresh on wake */
    }
    kick();
}

/* ------------------------------------------------------------------ */
/* bridge task                                                        */
/* ------------------------------------------------------------------ */

static void do_saves(void)
{
    settings_t *st = &s_cold->save;
    static char ota_url[sizeof(b.ota_url)];
    bool wifi, acc, cal, tm, ota, blf, vm, snd, feat, quiet;

    xSemaphoreTake(b.mtx, portMAX_DELAY);
    bool dnd_set = b.dnd_pending;
    bool dnd_val = b.dnd_value;
    b.dnd_pending = false;
    xSemaphoreGive(b.mtx);
    if (dnd_set) {
        phone_set_dnd(dnd_val);         /* persists the setting, applied live by the sip task */
    }

    xSemaphoreTake(b.mtx, portMAX_DELAY);
    wifi = b.wifi_pending;
    acc = b.acc_pending;
    cal = b.cal_pending;
    tm = b.time_pending;
    ota = b.ota_pending;
    blf = b.blf_pending;
    vm = b.vm_pending;
    snd = b.snd_pending;
    feat = b.feat_pending;
    quiet = b.quiet_pending;
    if (!wifi && !acc && !cal && !tm && !ota && !blf && !vm && !snd && !feat && !quiet) {
        xSemaphoreGive(b.mtx);
        return;
    }
    settings_get(st);
    if (wifi) {
        strlcpy(st->wifi_ssid, b.ssid, sizeof(st->wifi_ssid));
        if (b.wpass[0]) {
            strlcpy(st->wifi_pass, b.wpass, sizeof(st->wifi_pass));
        }
    }
    if (acc) {
        strlcpy(st->sip_server, b.server, sizeof(st->sip_server));
        st->sip_port = b.port;
        strlcpy(st->sip_user, b.user, sizeof(st->sip_user));
        if (b.apass[0]) {
            strlcpy(st->sip_password, b.apass, sizeof(st->sip_password));
        }
        strlcpy(st->sip_display_name, b.dname, sizeof(st->sip_display_name));
    }
    if (cal) {
        st->touch_cal.valid = true;
        st->touch_cal.x_min = b.cal.x_min;
        st->touch_cal.x_max = b.cal.x_max;
        st->touch_cal.y_min = b.cal.y_min;
        st->touch_cal.y_max = b.cal.y_max;
    }
    if (tm) {
        strlcpy(st->ntp_server, b.ntp, sizeof(st->ntp_server));
        strlcpy(st->tz, b.tz, sizeof(st->tz));
        st->clock_24h = b.clock_24h;
    }
    if (blf) {
        strlcpy(st->blf, b.blf, sizeof(st->blf));
    }
    if (vm) {
        strlcpy(st->vm_number, b.vm_number, sizeof(st->vm_number));
    }
    if (snd) {
        st->ring_tone = b.snd_tone;
        st->ring_volume = b.snd_ring;
        st->call_volume = b.snd_call;
        st->speaker_volume = b.snd_spk;
    }
    if (feat) {
        st->call_waiting = b.feat_cw;
        st->auto_answer = b.feat_aa;
        st->dnd = b.feat_dnd;
    }
    if (quiet) {
        st->quiet_hours = b.quiet_on;
        st->quiet_start = b.quiet_start;
        st->quiet_end = b.quiet_end;
        st->quiet_allow_contacts = b.quiet_allow;
    }
    if (ota) {
        strlcpy(ota_url, b.ota_url, sizeof(ota_url));
        strlcpy(st->ota_url, ota_url, sizeof(st->ota_url));   /* remember what was installed from */
    }
    b.wifi_pending = false;
    b.acc_pending = false;
    b.cal_pending = false;
    b.time_pending = false;
    b.ota_pending = false;
    b.blf_pending = false;
    b.vm_pending = false;
    b.snd_pending = false;
    b.feat_pending = false;
    b.quiet_pending = false;
    memset(b.wpass, 0, sizeof(b.wpass));
    memset(b.apass, 0, sizeof(b.apass));
    xSemaphoreGive(b.mtx);

    esp_err_t err = settings_set(st);   /* no-op when nothing changed */
    memset(st->wifi_pass, 0, sizeof(st->wifi_pass));
    memset(st->sip_password, 0, sizeof(st->sip_password));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "saving settings: %s", esp_err_to_name(err));
    } else if (wifi || acc || cal || tm || blf || vm || snd || feat || quiet) {
        ESP_LOGI(TAG, "settings saved from the UI:%s%s%s%s%s%s%s%s%s", wifi ? " wifi" : "", acc ? " account" : "",
                 cal ? " touch-calibration" : "", tm ? " time" : "", blf ? " blf" : "", vm ? " vm_number" : "",
                 snd ? " sounds" : "", feat ? " call-features" : "", quiet ? " quiet-hours" : "");
    }
    if (ota) {
        /* Progress and refusals (e.g. a call is active) come back through ota_get_status(). */
        err = ota_start(ota_url);
        ESP_LOGI(TAG, "update requested from the UI: %s", esp_err_to_name(err));
    }
}

static void refresh(void)
{
    ui_model_t *last = &s_cold->last, *model = &s_cold->model;
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

    ota_status_t os;
    ota_get_status(&os);
    ui_bridge_fill_ota(&loc, &os);

    phone_get_status(&ph);
    ui_bridge_build_model(&loc, &ph, esp_timer_get_time(), model);
    /* history: the rev tells an open History screen to refetch; the newest unseen missed call
     * names the home screen's notice (looked up only when the history changed) */
    static uint32_t hist_rev;
    static bool have_rev;
    static char missed_last[UI_NAME_LEN];
    static unsigned missed;
    uint32_t rev = dir_history_rev();
    if (!have_rev || rev != hist_rev) {
        static dir_call_t c;        /* bridge task only */
        missed = dir_history_unseen_missed();
        missed_last[0] = 0;
        if (dir_history_last_unseen_missed(&c)) {
            strlcpy(missed_last, c.name[0] ? c.name : c.number[0] ? c.number : "Unknown", sizeof(missed_last));
        }
        hist_rev = rev;
        have_rev = true;
    }
    model->history_rev = rev;
    model->missed_unseen = (uint8_t)(missed > 99 ? 99 : missed);
    strlcpy(model->missed_last, missed_last, sizeof(model->missed_last));
    model->now = loc.time_valid ? (int64_t)(now / 60) * 60 : 0;
    if (model->call != UI_CALL_IDLE && !model->remote_name[0] && model->remote_number[0]) {
        /* caller ID from the contacts when the far end sent no display name (and for outgoing
         * calls to a contact) */
        dir_contact_lookup(model->remote_number, model->remote_name, sizeof(model->remote_name));
    }
    if (have_last && memcmp(model, last, sizeof(*model)) == 0) {
        return;
    }
    lvgl_port_lock(0);
    ui_update(model);
    lvgl_port_unlock();
    *last = *model;
    have_last = true;
}

/* Refreshes on every message (events, keys, UI actions) and, while the backlight is on or
 * dimmed, once a second for the clock and call timer. While it is off the task sleeps until a
 * message: events that matter in the dark (incoming / missed call, registration, settings,
 * BLF, OTA) still refresh at once, so the incoming-call screen is drawn before the backlight
 * lights (display_wake runs first, then this refresh, within the panel's 120 ms start-up).
 * Nothing is lost meanwhile: every refresh rebuilds the whole model from b.loc, the phone
 * status, history and the clock, and DISPLAY_EVENT_BACKLIGHT (on) kicks one at wake-up. */
static void bridge_task(void *arg)
{
    (void)arg;
    int64_t next_tick = esp_timer_get_time() / 1000;
    for (;;) {
        int64_t now = esp_timer_get_time() / 1000;
        int64_t wait = next_tick - now;
        TickType_t ticks = wait > 0 ? pdMS_TO_TICKS(wait) : 0;
        if (!display_is_lit()) {
            ticks = portMAX_DELAY;      /* dark: no clock refresh, only messages */
        }
        bridge_msg_t m;
        if (xQueueReceive(b.q, &m, ticks) == pdTRUE) {
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
    s_cold = heap_caps_calloc_prefer(1, sizeof(*s_cold), 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(b.q && b.mtx && s_cold, ESP_ERR_NO_MEM, TAG, "no memory");

    b.time_mtx = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(b.time_mtx != NULL, ESP_ERR_NO_MEM, TAG, "no memory");
    apply_time_from_settings();     /* TZ now; SNTP starts at the first IP (or right below) */

    xSemaphoreTake(b.mtx, portMAX_DELAY);
    b.loc.net = net_is_connected() ? UI_NET_CONNECTED : UI_NET_OFF;
    strlcpy(b.loc.ip, net_ip_str(), sizeof(b.loc.ip));
    update_settings_locked();
    ui_bridge_fill_fw(&b.loc, esp_app_get_description()->version, ota_running_partition());
    xSemaphoreGive(b.mtx);

    lvgl_port_lock(0);
    ui_init(disp, &s_actions);
    lvgl_port_unlock();

    static const esp_event_base_t *const bases[] = {
        &SIPWIRE_NET_EVENT, &SIPWIRE_SIP_EVENT, &SIPWIRE_CALL_EVENT, &SIPWIRE_KEY_EVENT, &SIPWIRE_SETTINGS_EVENT,
        &SIPWIRE_OTA_EVENT, &SIPWIRE_BLF_EVENT, &SIPWIRE_DISPLAY_EVENT,
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
