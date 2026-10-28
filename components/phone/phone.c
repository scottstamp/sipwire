/* Phone: call state machine around the SIP UA. See phone.h.
 *
 * The "sip" task owns the UA (it is not thread safe): it polls the SIP
 * socket, ticks the UA, drains the command queue filled by the public API,
 * reacts to network/settings notifications and runs the timers (ENDED → IDLE,
 * DTMF spacing, volume persistence, UA re-creation). UA callbacks therefore
 * also run in the sip task.
 *
 * s.st (the status snapshot) and the volume fields are guarded by st_mtx so
 * phone_get_status / phone_set_* work from any task. Everything else in `s`
 * is sip-task only. */

#include "phone.h"

#include <stdatomic.h>
#include <string.h>
#include <arpa/inet.h>

#include "audio.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "net.h"
#include "settings.h"
#include "sip_ua.h"
#include "sipwire_events.h"

#include "phone_priv.h"

static const char *TAG = "phone";
static const char *SIP_TAG = "sip";

#define SIP_TASK_STACK          8192
#define SIP_TASK_PRIO           5
#define SIP_TASK_CORE           0
#define CMD_QUEUE_LEN           8
#define POLL_MS                 20
#define UNREG_WAIT_MS           2000
#define RETRY_MS                10000       /* DNS / socket / create failure */
#define VOLUME_SAVE_DELAY_MS    1000
#define SPEAKER_VOLUME          90
#define SIP_LOCAL_PORT          5060
#define SIP_KEEPALIVE_S         25
#define EVENT_POST_TIMEOUT      pdMS_TO_TICKS(50)
#define DEFAULT_REJECT_CODE     603

#define NOTIFY_NET_UP           BIT0
#define NOTIFY_NET_DOWN         BIT1
#define NOTIFY_SETTINGS         BIT2

typedef enum {
    CMD_DIAL,
    CMD_ANSWER,
    CMD_REJECT,
    CMD_HANGUP,
    CMD_HOLD,
    CMD_DTMF,
} cmd_type_t;

typedef struct {
    cmd_type_t type;
    int code;
    bool flag;
    char arg[64];
} phone_cmd_t;

static struct {
    bool inited;
    TaskHandle_t task;
    QueueHandle_t q;
    SemaphoreHandle_t st_mtx;
    _Atomic uint32_t net_ip;            /* host order, from NET_EVENT_GOT_IP */

    /* ---- under st_mtx ---- */
    phone_status_t st;
    sip_ua_call_state_t vstate;         /* call state for the volume policy */
    uint8_t ring_vol, call_vol, spk_vol;
    bool vol_dirty;
    uint64_t vol_save_at;

    /* ---- sip task only ---- */
    sip_ua_t *ua;
    sip_udp_t udp;
    uint32_t ua_ip;
    settings_t acc;                     /* account the UA was (last) built from */
    bool need_ua;
    uint64_t retry_at;
    bool closing;                       /* unregistering before re-creation */
    uint64_t close_deadline;
    sip_call_handle_t call;
    sip_ua_call_state_t state;
    bool outgoing;
    int end_code;
    uint64_t ended_until;
    phone_dtmf_seq_t dtmf;
} s = {
    .udp = { .fd = -1 },
};

static uint64_t now_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000);
}

static void st_lock(void)
{
    xSemaphoreTake(s.st_mtx, portMAX_DELAY);
}

static void st_unlock(void)
{
    xSemaphoreGive(s.st_mtx);
}

/* ------------------------------------------------------------------ */
/* enum mapping (explicit, no casts)                                  */
/* ------------------------------------------------------------------ */

static sip_reg_state_t map_reg(sip_ua_reg_state_t st)
{
    switch (st) {
    case SIP_UA_REG_REGISTERING: return SIP_REG_REGISTERING;
    case SIP_UA_REG_REGISTERED:  return SIP_REG_REGISTERED;
    case SIP_UA_REG_FAILED:      return SIP_REG_FAILED;
    case SIP_UA_REG_NONE:
    default:                     return SIP_REG_NONE;
    }
}

static call_state_t map_call(sip_ua_call_state_t st)
{
    switch (st) {
    case SIP_UA_CALL_DIALING:     return CALL_DIALING;
    case SIP_UA_CALL_RINGING_OUT: return CALL_RINGING_OUT;
    case SIP_UA_CALL_RINGING_IN:  return CALL_RINGING_IN;
    case SIP_UA_CALL_ACTIVE:      return CALL_ACTIVE;
    case SIP_UA_CALL_HELD:        return CALL_HELD;
    case SIP_UA_CALL_ENDED:       return CALL_ENDED;
    case SIP_UA_CALL_IDLE:
    default:                      return CALL_IDLE;
    }
}

static esp_err_t map_sip_err(sip_err_t e)
{
    switch (e) {
    case SIP_OK:            return ESP_OK;
    case SIP_ERR_ARG:       return ESP_ERR_INVALID_ARG;
    case SIP_ERR_NOMEM:     return ESP_ERR_NO_MEM;
    case SIP_ERR_STATE:     return ESP_ERR_INVALID_STATE;
    case SIP_ERR_BUSY:      return ESP_ERR_INVALID_STATE;
    case SIP_ERR_NOT_FOUND: return ESP_ERR_NOT_FOUND;
    case SIP_ERR_OVERFLOW:  return ESP_ERR_INVALID_SIZE;
    case SIP_ERR_TRANSPORT:
    default:                return ESP_FAIL;
    }
}

/* ------------------------------------------------------------------ */
/* events, volume, audio                                              */
/* ------------------------------------------------------------------ */

static const char *reg_state_name(sip_reg_state_t st)
{
    switch (st) {
    case SIP_REG_REGISTERING: return "registering";
    case SIP_REG_REGISTERED:  return "registered";
    case SIP_REG_FAILED:      return "failed";
    case SIP_REG_NONE:
    default:                  return "none";
    }
}

static const char *call_state_name(sip_ua_call_state_t st)
{
    switch (st) {
    case SIP_UA_CALL_DIALING:     return "dialing";
    case SIP_UA_CALL_RINGING_OUT: return "ringing-out";
    case SIP_UA_CALL_RINGING_IN:  return "ringing-in";
    case SIP_UA_CALL_ACTIVE:      return "active";
    case SIP_UA_CALL_HELD:        return "held";
    case SIP_UA_CALL_ENDED:       return "ended";
    case SIP_UA_CALL_IDLE:
    default:                      return "idle";
    }
}

static void post_reg(sip_reg_state_t state, int code)
{
    sip_event_reg_t ev = { .state = state, .status_code = code };
    esp_err_t err = esp_event_post(SIPWIRE_SIP_EVENT, SIP_EVENT_REG_STATE, &ev, sizeof(ev), EVENT_POST_TIMEOUT);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "post reg event: %s", esp_err_to_name(err));
    }
}

static void set_reg(sip_reg_state_t state, int code)
{
    st_lock();
    bool changed = s.st.reg != state || s.st.reg_code != code;
    s.st.reg = state;
    s.st.reg_code = code;
    st_unlock();
    if (changed) {
        ESP_LOGI(TAG, "registration: %s (%d)", reg_state_name(state), code);
        post_reg(state, code);
    }
}

/* NONE without a SIP status (UA gone / no account); no event if already NONE. */
static void set_reg_none(void)
{
    st_lock();
    bool was_none = s.st.reg == SIP_REG_NONE;
    st_unlock();
    if (!was_none) {
        set_reg(SIP_REG_NONE, 0);
    }
}

/* Caller holds st_mtx. */
static uint8_t *current_volume_locked(void)
{
    switch (phone_volume_kind(s.vstate, s.st.speaker)) {
    case PHONE_VOL_RING:    return &s.ring_vol;
    case PHONE_VOL_SPEAKER: return &s.spk_vol;
    case PHONE_VOL_CALL:
    default:                return &s.call_vol;
    }
}

static void apply_volume(void)
{
    st_lock();
    uint8_t v = *current_volume_locked();
    s.st.volume = v;
    st_unlock();
    audio_set_volume(v);
}

/* Publish s.state (+ number/name/end_code already in s.st) to everyone:
 * status snapshot, media flags, tones/streaming, volume, CALL_EVENT. */
static void publish_call_state(void)
{
    call_event_state_t ev;
    memset(&ev, 0, sizeof(ev));         /* compared bytewise below */
    ev.state = map_call(s.state);
    ev.end_code = s.end_code;

    st_lock();
    s.vstate = s.state;
    s.st.call = ev.state;
    s.st.outgoing = s.outgoing;
    s.st.end_code = s.end_code;
    strlcpy(ev.number, s.st.number, sizeof(ev.number));
    strlcpy(ev.name, s.st.name, sizeof(ev.name));
    st_unlock();

    phone_media_set_state(s.state);
    bool early = phone_media_early();
    bool stream = phone_media_active() &&
                  (s.state == SIP_UA_CALL_ACTIVE || s.state == SIP_UA_CALL_HELD ||
                   ((s.state == SIP_UA_CALL_DIALING || s.state == SIP_UA_CALL_RINGING_OUT) && early));
    phone_media_audio(stream, phone_tone_for(s.state, s.outgoing, early, s.end_code));
    apply_volume();

    /* media updates re-run this without a state change: post only news */
    static call_event_state_t last;
    static bool posted;
    if (posted && memcmp(&last, &ev, sizeof(ev)) == 0) {
        return;
    }
    if (!posted || last.state != ev.state) {
        if (s.state == SIP_UA_CALL_ENDED) {
            ESP_LOGI(TAG, "call: ended %s (code %d)", ev.number, s.end_code);
        } else {
            ESP_LOGI(TAG, "call: %s%s%s", call_state_name(s.state), ev.number[0] ? " " : "", ev.number);
        }
    }
    esp_err_t err = esp_event_post(SIPWIRE_CALL_EVENT, CALL_EVENT_STATE, &ev, sizeof(ev), EVENT_POST_TIMEOUT);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "post call event: %s", esp_err_to_name(err));
        return;
    }
    last = ev;
    posted = true;
}

static void reset_call_audio_prefs(void)
{
    st_lock();
    s.st.muted = false;
    s.st.speaker = false;
    st_unlock();
    audio_set_mic_mute(false);
}

/* The call is over (UA said ENDED, or the UA went away under it). */
static void call_ended(int end_code)
{
    phone_media_teardown();
    phone_dtmf_seq_clear(&s.dtmf);
    reset_call_audio_prefs();
    s.call = 0;
    s.state = SIP_UA_CALL_ENDED;
    s.end_code = end_code;
    s.ended_until = now_ms() + phone_ended_hold_ms(s.outgoing, end_code);
    publish_call_state();
}

static void go_idle(void)
{
    s.state = SIP_UA_CALL_IDLE;
    s.end_code = 0;
    s.outgoing = false;
    st_lock();
    s.st.number[0] = '\0';
    s.st.name[0] = '\0';
    s.st.call_start_us = 0;
    st_unlock();
    publish_call_state();
}

/* ------------------------------------------------------------------ */
/* UA callbacks (sip task)                                            */
/* ------------------------------------------------------------------ */

static void ua_log(void *ctx, sip_log_level_t level, const char *msg)
{
    (void)ctx;
    switch (level) {
    case SIP_LOG_ERROR:   ESP_LOGE(SIP_TAG, "%s", msg); break;
    case SIP_LOG_WARN:    ESP_LOGW(SIP_TAG, "%s", msg); break;
    case SIP_LOG_INFO:    ESP_LOGI(SIP_TAG, "%s", msg); break;
    case SIP_LOG_DEBUG:   ESP_LOGD(SIP_TAG, "%s", msg); break;
    case SIP_LOG_VERBOSE: ESP_LOGV(SIP_TAG, "%s", msg); break;
    case SIP_LOG_NONE:
    default:              break;
    }
}

/* UA log level from the runtime level of the "sip" tag (capped at what is
 * compiled in), so `esp_log_level_set("sip", ...)` + a re-register selects it. */
static sip_log_level_t ua_log_level(void)
{
    esp_log_level_t l = esp_log_level_get(SIP_TAG);
    if (l > CONFIG_LOG_MAXIMUM_LEVEL) {
        l = CONFIG_LOG_MAXIMUM_LEVEL;
    }
    switch (l) {
    case ESP_LOG_NONE:
    case ESP_LOG_ERROR:   return SIP_LOG_ERROR;
    case ESP_LOG_WARN:    return SIP_LOG_WARN;
    case ESP_LOG_INFO:    return SIP_LOG_INFO;
    case ESP_LOG_DEBUG:   return SIP_LOG_DEBUG;
    case ESP_LOG_VERBOSE:
    default:              return SIP_LOG_VERBOSE;
    }
}

static uint32_t ua_rand(void *ctx)
{
    (void)ctx;
    return esp_random();
}

static void on_reg(void *ctx, sip_ua_reg_state_t state, int code)
{
    (void)ctx;
    set_reg(map_reg(state), code);
}

static void on_incoming(void *ctx, sip_call_handle_t call, const char *number, const char *name)
{
    (void)ctx;
    ESP_LOGI(TAG, "incoming call from %s \"%s\"", number, name);
    s.call = call;
    s.outgoing = false;
    s.end_code = 0;
    phone_dtmf_seq_clear(&s.dtmf);
    st_lock();
    strlcpy(s.st.number, number ? number : "", sizeof(s.st.number));
    strlcpy(s.st.name, name ? name : "", sizeof(s.st.name));
    s.st.call_start_us = 0;
    st_unlock();
    /* state follows with call_state(RINGING_IN) */
}

static void on_call_state(void *ctx, sip_call_handle_t call, const sip_call_info_t *info)
{
    (void)ctx;
    if (call != s.call || s.call == 0) {
        ESP_LOGD(TAG, "ignoring state %d of stale call %d", (int)info->state, call);
        return;
    }
    st_lock();
    if (info->number && info->number[0]) {
        strlcpy(s.st.number, info->number, sizeof(s.st.number));
    }
    if (info->display_name && info->display_name[0]) {
        strlcpy(s.st.name, info->display_name, sizeof(s.st.name));
    }
    if (info->state == SIP_UA_CALL_ACTIVE && s.st.call_start_us == 0) {
        s.st.call_start_us = esp_timer_get_time();
    }
    st_unlock();

    if (info->state == SIP_UA_CALL_ENDED) {
        call_ended(phone_end_code(info->end_reason, info->status_code));
        return;
    }
    s.state = info->state;
    publish_call_state();
}

static void on_media(void *ctx, sip_call_handle_t call, const sip_media_info_t *mi)
{
    (void)ctx;
    if (call != s.call || s.call == 0) {
        return;
    }
    phone_media_update(mi);
    publish_call_state();       /* early media → stop ringback, start stream; hold → flags */
}

/* ------------------------------------------------------------------ */
/* UA lifecycle (sip task)                                            */
/* ------------------------------------------------------------------ */

static bool account_changed(const settings_t *a, const settings_t *b)
{
    return strcmp(a->sip_server, b->sip_server) != 0 || a->sip_port != b->sip_port ||
           a->sip_transport != b->sip_transport || strcmp(a->sip_user, b->sip_user) != 0 ||
           strcmp(a->sip_auth_user, b->sip_auth_user) != 0 || strcmp(a->sip_password, b->sip_password) != 0 ||
           strcmp(a->sip_display_name, b->sip_display_name) != 0 || a->sip_reg_expires != b->sip_reg_expires;
}

static void ua_destroy(void)
{
    if (s.ua == NULL) {
        return;
    }
    if (s.call != 0) {
        ESP_LOGW(TAG, "SIP stack restarted during a call");
        call_ended(0);
    }
    sip_ua_destroy(s.ua);
    s.ua = NULL;
    s.closing = false;
    sip_udp_close(&s.udp);
    st_lock();
    s.st.local_addr[0] = '\0';
    st_unlock();
    set_reg_none();                     /* destroyed without a final un-REGISTER */
}

static void ua_retry_later(uint64_t now)
{
    s.need_ua = true;
    s.retry_at = now + RETRY_MS;
}

static void ua_create(uint64_t now)
{
    s.need_ua = false;
    settings_get(&s.acc);
    uint32_t net_ip = atomic_load(&s.net_ip);

    st_lock();
    if (s.acc.sip_server[0]) {
        snprintf(s.st.server, sizeof(s.st.server), "%.48s:%u", s.acc.sip_server, s.acc.sip_port);
    } else {
        s.st.server[0] = '\0';
    }
    st_unlock();

    if (net_ip == 0) {
        return;                         /* created on GOT_IP */
    }
    if (s.acc.sip_server[0] == '\0' || s.acc.sip_user[0] == '\0') {
        ESP_LOGI(TAG, "no SIP account configured (cfg set sip_server / sip_user)");
        set_reg_none();
        return;
    }
    if (s.acc.sip_transport != SIP_TRANSPORT_UDP) {
        ESP_LOGW(TAG, "only UDP is supported, using UDP");
    }

    sip_addr_t srv;
    if (sip_udp_resolve(s.acc.sip_server, s.acc.sip_port, &srv) != SIP_OK) {
        ESP_LOGW(TAG, "cannot resolve %s, retry in %u s", s.acc.sip_server, RETRY_MS / 1000);
        set_reg(SIP_REG_FAILED, 0);
        ua_retry_later(now);
        return;
    }
    if (sip_udp_open(&s.udp, SIP_LOCAL_PORT) != SIP_OK && sip_udp_open(&s.udp, 0) != SIP_OK) {
        ESP_LOGE(TAG, "cannot open SIP UDP socket");
        set_reg(SIP_REG_FAILED, 0);
        ua_retry_later(now);
        return;
    }
    uint32_t lip;
    if (sip_udp_local_ip_for(&srv, &lip) != SIP_OK || lip == 0) {
        lip = net_ip;
    }

    const sip_ua_config_t cfg = {
        .server_host = s.acc.sip_server,
        .server_port = s.acc.sip_port,
        .server_addr = srv,
        .user = s.acc.sip_user,
        .auth_user = s.acc.sip_auth_user,
        .password = s.acc.sip_password,
        .display_name = s.acc.sip_display_name,
        .expires = s.acc.sip_reg_expires,
        .local_addr = { .ip = lip, .port = s.udp.port },
        .rtp_port_alloc = phone_media_alloc_port,
        .keepalive_s = SIP_KEEPALIVE_S,
        .keepalive_mode = SIP_KEEPALIVE_CRLF,
        .send = sip_udp_send,
        .send_ctx = &s.udp,
        .rand = ua_rand,
        .log = ua_log,
        .log_level = ua_log_level(),
        .cb = {
            .reg_state = on_reg,
            .incoming_call = on_incoming,
            .call_state = on_call_state,
            .media = on_media,
        },
    };
    sip_err_t err = sip_ua_create(&cfg, &s.ua);
    if (err != SIP_OK) {
        ESP_LOGE(TAG, "sip_ua_create: %d", (int)err);
        s.ua = NULL;
        sip_udp_close(&s.udp);
        set_reg(SIP_REG_FAILED, 0);
        ua_retry_later(now);
        return;
    }
    s.ua_ip = lip;
    phone_media_set_server_ip(srv.ip);

    char ip[16];
    sip_ip_format(lip, ip);
    st_lock();
    snprintf(s.st.local_addr, sizeof(s.st.local_addr), "%s:%u", ip, s.udp.port);
    st_unlock();
    /* user and server only: never the password */
    ESP_LOGI(TAG, "SIP account %s@%s:%u, local %s:%u", s.acc.sip_user, s.acc.sip_server, s.acc.sip_port, ip,
             s.udp.port);

    sip_ua_tick(s.ua, now);
    err = sip_ua_register(s.ua);
    if (err != SIP_OK) {
        ESP_LOGE(TAG, "sip_ua_register: %d", (int)err);
    }
}

/* Start a clean shutdown of the UA (hang up, unregister, bounded wait) and
 * re-create it afterwards. */
static void ua_restart(uint64_t now)
{
    if (s.ua == NULL) {
        s.need_ua = true;
        s.retry_at = 0;
        return;
    }
    if (s.call != 0) {
        sip_call_hangup(s.ua, s.call);  /* reports ENDED synchronously */
    }
    if (sip_ua_reg_state(s.ua) == SIP_UA_REG_REGISTERED && sip_ua_unregister(s.ua) == SIP_OK) {
        s.closing = true;
        s.close_deadline = now + UNREG_WAIT_MS;
    } else {
        ua_destroy();
        s.need_ua = true;
        s.retry_at = 0;
    }
}

static void on_net_up(uint64_t now)
{
    uint32_t ip = atomic_load(&s.net_ip);
    if (s.ua && !s.closing && s.call != 0 && ip == s.ua_ip) {
        /* same address and a call in progress: keep the call alive */
        const sip_addr_t la = { .ip = ip, .port = s.udp.port };
        sip_ua_set_local_addr(s.ua, &la);
        return;
    }
    ua_destroy();
    s.need_ua = true;
    s.retry_at = 0;
    (void)now;
}

static void on_settings(uint64_t now)
{
    static settings_t ns;               /* sip task only; keep it off the stack */
    settings_get(&ns);

    st_lock();
    if (!s.vol_dirty) {
        s.ring_vol = ns.ring_volume;
        s.call_vol = ns.call_volume;
    }
    st_unlock();
    apply_volume();

    if (account_changed(&ns, &s.acc)) {
        ESP_LOGI(TAG, "SIP account changed, re-registering");
        s.acc = ns;
        ua_restart(now);
    }
}

/* ------------------------------------------------------------------ */
/* commands (sip task)                                                */
/* ------------------------------------------------------------------ */

static void cmd_dial(const char *number)
{
    if (s.ua == NULL || s.closing) {
        ESP_LOGW(TAG, "dial %s: no SIP account / network", number);
        return;
    }
    if (s.call != 0) {
        ESP_LOGW(TAG, "dial %s: a call is already in progress", number);
        return;
    }
    s.outgoing = true;
    s.end_code = 0;
    phone_dtmf_seq_clear(&s.dtmf);
    st_lock();
    strlcpy(s.st.number, number, sizeof(s.st.number));
    s.st.name[0] = '\0';
    s.st.call_start_us = 0;
    st_unlock();

    s.call = 0;
    sip_err_t err = sip_call_dial(s.ua, number, &s.call);   /* DIALING is delivered inside */
    if (err != SIP_OK) {
        s.call = 0;
        ESP_LOGW(TAG, "dial %s failed: %s", number, esp_err_to_name(map_sip_err(err)));
    }
}

static void handle_cmd(const phone_cmd_t *c, uint64_t now)
{
    sip_err_t err = SIP_OK;
    switch (c->type) {
    case CMD_DIAL:
        cmd_dial(c->arg);
        return;
    case CMD_ANSWER:
        if (s.ua && s.call && s.state == SIP_UA_CALL_RINGING_IN) {
            err = sip_call_answer(s.ua, s.call);
        } else {
            ESP_LOGW(TAG, "answer: no ringing call");
        }
        break;
    case CMD_REJECT:
        if (s.ua && s.call && s.state == SIP_UA_CALL_RINGING_IN) {
            err = sip_call_reject(s.ua, s.call, c->code);
        } else {
            ESP_LOGW(TAG, "reject: no ringing call");
        }
        break;
    case CMD_HANGUP:
        if (s.ua && s.call) {
            err = sip_call_hangup(s.ua, s.call);
        } else if (s.state == SIP_UA_CALL_ENDED) {
            go_idle();
        }
        break;
    case CMD_HOLD:
        if (s.ua && s.call &&
            ((c->flag && s.state == SIP_UA_CALL_ACTIVE) || (!c->flag && s.state == SIP_UA_CALL_HELD))) {
            err = sip_call_hold(s.ua, s.call, c->flag);
        } else {
            ESP_LOGW(TAG, "%s: no %s call", c->flag ? "hold" : "resume", c->flag ? "active" : "held");
        }
        break;
    case CMD_DTMF:
        if (s.state != SIP_UA_CALL_ACTIVE) {
            ESP_LOGW(TAG, "dtmf: no active call");
        } else if (!phone_dtmf_seq_push(&s.dtmf, c->arg)) {
            ESP_LOGW(TAG, "dtmf: queue full");
        }
        break;
    }
    if (err != SIP_OK) {
        ESP_LOGW(TAG, "command %d failed: %s", (int)c->type, esp_err_to_name(map_sip_err(err)));
    }
    (void)now;
}

/* ------------------------------------------------------------------ */
/* sip task                                                           */
/* ------------------------------------------------------------------ */

static void housekeeping(uint64_t now)
{
    /* account change: wait for the un-REGISTER (bounded), then rebuild */
    if (s.closing && (sip_ua_reg_state(s.ua) == SIP_UA_REG_NONE || now >= s.close_deadline)) {
        ua_destroy();
        s.need_ua = true;
        s.retry_at = 0;
    }
    if (s.need_ua && s.ua == NULL && now >= s.retry_at) {
        ua_create(now);
    }

    if (s.state == SIP_UA_CALL_ENDED && now >= s.ended_until) {
        go_idle();
    }

    char d = phone_dtmf_seq_due(&s.dtmf, now);
    if (d) {
        phone_dtmf_res_t r = phone_media_send_dtmf(d);
        if (r != PHONE_DTMF_BUSY) {
            phone_dtmf_seq_consume(&s.dtmf, now);
            audio_play_dtmf(d);
            if (r == PHONE_DTMF_UNAVAILABLE) {
                ESP_LOGW(TAG, "dtmf %c: peer has no telephone-event (or not sending)", d);
            }
        }
    }

    bool save = false;
    uint8_t ring = 0, call = 0;
    st_lock();
    if (s.vol_dirty && now >= s.vol_save_at) {
        s.vol_dirty = false;
        save = true;
        ring = s.ring_vol;
        call = s.call_vol;
    }
    st_unlock();
    if (save) {
        static settings_t cur;          /* sip task only */
        settings_get(&cur);
        if (cur.ring_volume != ring || cur.call_volume != call) {
            cur.ring_volume = ring;
            cur.call_volume = call;
            esp_err_t err = settings_set(&cur);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "saving volume: %s", esp_err_to_name(err));
            }
        }
    }
}

static void sip_task(void *arg)
{
    (void)arg;
    for (;;) {
        uint32_t bits = 0;
        xTaskNotifyWait(0, UINT32_MAX, &bits, 0);
        uint64_t now = now_ms();
        if (bits & NOTIFY_NET_UP) {
            on_net_up(now);
        }
        if (bits & NOTIFY_SETTINGS) {
            on_settings(now);
        }
        /* NOTIFY_NET_DOWN: keep the UA; it retries and is rebuilt on GOT_IP */

        phone_cmd_t cmd;
        bool have_cmd = false;
        if (s.ua) {
            if (sip_udp_poll(&s.udp, s.ua, POLL_MS) < 0) {
                vTaskDelay(pdMS_TO_TICKS(POLL_MS));     /* socket error: don't spin */
            }
            sip_ua_tick(s.ua, now_ms());
        } else {
            have_cmd = xQueueReceive(s.q, &cmd, pdMS_TO_TICKS(POLL_MS)) == pdTRUE;
        }

        now = now_ms();
        while (have_cmd || xQueueReceive(s.q, &cmd, 0) == pdTRUE) {
            have_cmd = false;
            handle_cmd(&cmd, now);
        }
        housekeeping(now);
    }
}

/* ------------------------------------------------------------------ */
/* event handlers (default event loop task): only notify the sip task */
/* ------------------------------------------------------------------ */

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == SIPWIRE_NET_EVENT && id == NET_EVENT_GOT_IP && data) {
        const net_event_ip_t *ev = data;
        atomic_store(&s.net_ip, ntohl(ev->ip));
        xTaskNotify(s.task, NOTIFY_NET_UP, eSetBits);
    } else if (base == SIPWIRE_NET_EVENT && id == NET_EVENT_DISCONNECTED) {
        atomic_store(&s.net_ip, 0);     /* no new UA until the next GOT_IP */
        xTaskNotify(s.task, NOTIFY_NET_DOWN, eSetBits);
    } else if (base == SIPWIRE_SETTINGS_EVENT && id == SETTINGS_EVENT_CHANGED) {
        xTaskNotify(s.task, NOTIFY_SETTINGS, eSetBits);
    }
}

/* ------------------------------------------------------------------ */
/* public API                                                         */
/* ------------------------------------------------------------------ */

esp_err_t phone_init(void)
{
    ESP_RETURN_ON_FALSE(!s.inited, ESP_ERR_INVALID_STATE, TAG, "already initialised");
    s.st_mtx = xSemaphoreCreateMutex();
    s.q = xQueueCreate(CMD_QUEUE_LEN, sizeof(phone_cmd_t));
    ESP_RETURN_ON_FALSE(s.st_mtx && s.q, ESP_ERR_NO_MEM, TAG, "no memory");
    ESP_RETURN_ON_ERROR(phone_media_init(), TAG, "media init");

    settings_get(&s.acc);
    s.ring_vol = s.acc.ring_volume;
    s.call_vol = s.acc.call_volume;
    s.spk_vol = SPEAKER_VOLUME;
    s.st.reg = SIP_REG_NONE;
    s.st.call = CALL_IDLE;
    s.state = SIP_UA_CALL_IDLE;
    s.vstate = SIP_UA_CALL_IDLE;
    s.st.volume = s.ring_vol;
    s.need_ua = true;
    audio_set_volume(s.st.volume);

    BaseType_t ok = xTaskCreatePinnedToCore(sip_task, "sip", SIP_TASK_STACK, NULL, SIP_TASK_PRIO, &s.task,
                                            SIP_TASK_CORE);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "sip task");
    s.inited = true;

    ESP_RETURN_ON_ERROR(esp_event_handler_register(SIPWIRE_NET_EVENT, ESP_EVENT_ANY_ID, on_event, NULL), TAG,
                        "net handler");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(SIPWIRE_SETTINGS_EVENT, ESP_EVENT_ANY_ID, on_event, NULL),
                        TAG, "settings handler");

    /* Wi-Fi may already be up (GOT_IP posted before we registered). */
    uint32_t ip;
    if (net_is_connected() && sip_ip_parse(net_ip_str(), &ip)) {
        atomic_store(&s.net_ip, ip);
        xTaskNotify(s.task, NOTIFY_NET_UP, eSetBits);
    }

    ESP_RETURN_ON_ERROR(phone_console_register(), TAG, "console commands");
    ESP_LOGI(TAG, "ready");
    return ESP_OK;
}

static esp_err_t send_cmd(const phone_cmd_t *c)
{
    ESP_RETURN_ON_FALSE(s.inited, ESP_ERR_INVALID_STATE, TAG, "not initialised");
    if (xQueueSend(s.q, c, 0) != pdTRUE) {
        ESP_LOGW(TAG, "command queue full");
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t phone_dial(const char *number)
{
    phone_cmd_t c = { .type = CMD_DIAL };
    ESP_RETURN_ON_FALSE(number && number[0] && strlen(number) < sizeof(c.arg), ESP_ERR_INVALID_ARG, TAG,
                        "bad number");
    strlcpy(c.arg, number, sizeof(c.arg));
    return send_cmd(&c);
}

esp_err_t phone_answer(void)
{
    const phone_cmd_t c = { .type = CMD_ANSWER };
    return send_cmd(&c);
}

esp_err_t phone_hangup(void)
{
    const phone_cmd_t c = { .type = CMD_HANGUP };
    return send_cmd(&c);
}

esp_err_t phone_reject(int code)
{
    if (code == 0) {
        code = DEFAULT_REJECT_CODE;
    }
    ESP_RETURN_ON_FALSE(code >= 400 && code <= 699, ESP_ERR_INVALID_ARG, TAG, "reject code must be 4xx-6xx");
    const phone_cmd_t c = { .type = CMD_REJECT, .code = code };
    return send_cmd(&c);
}

esp_err_t phone_dtmf_string(const char *digits)
{
    phone_cmd_t c = { .type = CMD_DTMF };
    ESP_RETURN_ON_FALSE(digits && digits[0] && strlen(digits) < sizeof(c.arg) &&
                        strlen(digits) <= PHONE_DTMF_QUEUE, ESP_ERR_INVALID_ARG, TAG, "bad digits");
    for (const char *p = digits; *p; p++) {
        ESP_RETURN_ON_FALSE(phone_dtmf_valid(*p), ESP_ERR_INVALID_ARG, TAG, "bad DTMF digit '%c'", *p);
    }
    strlcpy(c.arg, digits, sizeof(c.arg));
    return send_cmd(&c);
}

esp_err_t phone_dtmf(char digit)
{
    const char str[2] = { digit, '\0' };
    return phone_dtmf_string(str);
}

esp_err_t phone_hold(bool hold)
{
    const phone_cmd_t c = { .type = CMD_HOLD, .flag = hold };
    return send_cmd(&c);
}

void phone_set_mute(bool muted)
{
    if (!s.inited) {
        return;
    }
    st_lock();
    s.st.muted = muted;
    st_unlock();
    audio_set_mic_mute(muted);
}

void phone_set_speaker(bool on)
{
    if (!s.inited) {
        return;
    }
    st_lock();
    s.st.speaker = on;
    st_unlock();
    apply_volume();
}

void phone_set_volume(uint8_t volume)
{
    if (!s.inited) {
        return;
    }
    if (volume > 100) {
        volume = 100;
    }
    st_lock();
    *current_volume_locked() = volume;
    if (phone_volume_kind(s.vstate, s.st.speaker) != PHONE_VOL_SPEAKER) {
        s.vol_dirty = true;
        s.vol_save_at = now_ms() + VOLUME_SAVE_DELAY_MS;
    }
    s.st.volume = volume;
    st_unlock();
    audio_set_volume(volume);
}

void phone_get_status(phone_status_t *out)
{
    if (!s.inited) {
        memset(out, 0, sizeof(*out));
        out->reg = SIP_REG_NONE;
        out->call = CALL_IDLE;
        return;
    }
    st_lock();
    *out = s.st;
    st_unlock();
    phone_media_status(out);
}
