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
 * is sip-task only.
 *
 * Two lines: the foreground call lives in s.call / s.state / s.outgoing /
 * s.line / s.fg_media (+ number, name, start time in s.st) as before; the
 * background call (waiting or held) in s.bg. Swapping exchanges them
 * (fg_save / fg_load) and moves the single media stream. */

#include "phone.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>

#include "audio.h"
#include "directory.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_vfs_eventfd.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "net.h"
#include "power.h"
#include "settings.h"
#include "sip_ua.h"
#include "sipwire_events.h"

#include "phone_priv.h"

static const char *TAG = "phone";
static const char *SIP_TAG = "sip";

#define SIP_TASK_STACK          7168    /* worst seen 4.7 KB (QEMU smoke: transfers, call waiting, BLF) */
#define SIP_TASK_PRIO           5
#define SIP_TASK_CORE           0
#define CMD_QUEUE_LEN           8
#define POLL_MS                 20          /* fallback wait without the wake eventfd; error back-off */
#define MAX_WAIT_MS             2000        /* sip task: longest block even with no deadline (safety net) */
#define DTMF_BUSY_RETRY_MS      20          /* previous RFC 4733 event still being sent */
#define SPIN_LIMIT              16          /* consecutive zero waits before forcing a short sleep */
#define SPIN_WAIT_MS            10
#define UNREG_WAIT_MS           2000
#define RETRY_MS                10000       /* DNS / socket / create failure */
#define VOLUME_SAVE_DELAY_MS    3000    /* debounce: one NVS write per burst of +/- presses */
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
    CMD_ANSWER_WAITING,
    CMD_REJECT_WAITING,
    CMD_SWAP,
    CMD_XFER_BLIND,
    CMD_XFER_START,
    CMD_XFER_COMPLETE,
    CMD_XFER_CANCEL,
} cmd_type_t;

#define AUTO_ANSWER_MAX_S       60          /* longer answer-after delays are capped */
#define WAITING_REJECT_CODE     486
#define INFO_DTMF_DURATION_MS   160

/* One call line (the background call, or a saved foreground call). */
typedef struct {
    sip_call_handle_t h;                /* 0 = none */
    sip_ua_call_state_t state;
    bool outgoing;
    int answer_after;
    uint8_t line;
    char number[64];
    char name[64];
    int64_t start_us;
    bool rej;                           /* the user declined it (call log) */
    bool media_valid;
    sip_media_info_t media;
} phone_line_t;

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
    int wake_fd;                        /* eventfd: ends the sip task's select() early, -1 = none */
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
    uint8_t line;                       /* foreground line number, 0 = none */
    bool rej;                           /* foreground: the user declined the ringing call (call log) */
    int answer_after;                   /* foreground: caller asked for auto answer, -1 = no */
    bool aa_pending;                    /* auto answer due at aa_at */
    uint64_t aa_at;
    bool fg_media_valid;                /* last media of the foreground call */
    sip_media_info_t fg_media;
    phone_line_t bg;                    /* background call (bg.h == 0: none) */
    /* transfer: the call being transferred and the consultation call */
    sip_call_handle_t xfer_h, xfer_consult;
    /* call features (settings) */
    bool f_dnd, f_cw, f_aa;
    /* BLF: the parsed `blf` setting and one subscription per entry */
    char blf_list[SIPWIRE_BLF_LIST_MAX];
    sipwire_blf_entry_t blf[SIPWIRE_BLF_MAX];
    int blf_n;
    sip_sub_handle_t blf_h[SIPWIRE_BLF_MAX];
} s = {
    .udp = { .fd = -1 },
    .wake_fd = -1,
};

/* Scratch settings_t (~1.3 KB) for on_settings() and the volume save: sip task only, both
 * rare, so in PSRAM when there is some (internal RAM is the scarce resource). */
static settings_t *s_cfg;

static uint64_t now_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000);
}

/* The sip task blocks in select() until its next deadline; anything another task hands it
 * (a command, a notify bit, a new deadline) must end that wait. Any task, not an ISR. */
static void wake_sip_task(void)
{
    if (s.wake_fd >= 0) {
        const uint64_t one = 1;
        (void)!write(s.wake_fd, &one, sizeof(one));
    }
}

static void notify_sip_task(uint32_t bits)
{
    xTaskNotify(s.task, bits, eSetBits);
    wake_sip_task();
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

/* Settings `aec`, `aec_suppress`, `aec_suppress_act`, live: the audio
 * component allocates the canceller on first use. */
static void apply_aec(const settings_t *st)
{
    esp_err_t err = ESP_OK;
    if (st->aec) {
        err = audio_set_aec_suppress(st->aec_suppress, st->aec_suppress_act, 0);
    }
    if (err == ESP_OK) {
        err = audio_set_aec(st->aec);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "aec %s: %s", st->aec ? "on" : "off", esp_err_to_name(err));
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

/* Power policy: from the first ring (or dial) of a call until no call is left, Wi-Fi power
 * save is off (DTIM bursts stutter RTP) and the CPU stays at full speed (G.722, AEC). That
 * covers ringing, active, held, a second (waiting or held) call and the ended screen.
 * Directory flash writes (history of a rejected / ended background call, contact edits) wait
 * for the same span: a flash erase stalls the audio tasks past the I2S ring. */
static void update_power(void)
{
    bool busy = s.state != SIP_UA_CALL_IDLE || s.bg.h != 0;
    net_set_low_latency(busy);
    power_hold(POWER_HOLD_CALL, busy);
    dir_defer_writes(busy);
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

    st_lock();
    s.st.line = s.line;
    st_unlock();

    update_power();                     /* before media/tones: PS off ahead of RTP */
    phone_media_set_state(s.state);
    bool early = phone_media_early();
    bool stream = phone_media_active() &&
                  (s.state == SIP_UA_CALL_ACTIVE || s.state == SIP_UA_CALL_HELD ||
                   ((s.state == SIP_UA_CALL_DIALING || s.state == SIP_UA_CALL_RINGING_OUT) && early));
    phone_tone_t tone = phone_tone_for(s.state, s.outgoing, early, s.end_code);
    tone = phone_tone_waiting(tone, s.state, s.bg.h ? s.bg.state : SIP_UA_CALL_IDLE);
    phone_media_audio(stream, tone);
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

/* ---- second line ---- */

/* Status snapshot of the background call, and CALL_EVENT_SECOND with `state`
 * (the line's own state, CALL_ENDED when it went away, CALL_IDLE when it was
 * promoted to the foreground). */
static void post_second(call_state_t state, int end_code, const char *number, const char *name)
{
    st_lock();
    if (s.bg.h) {
        s.st.second.call = map_call(s.bg.state);
        s.st.second.outgoing = s.bg.outgoing;
        strlcpy(s.st.second.number, s.bg.number, sizeof(s.st.second.number));
        strlcpy(s.st.second.name, s.bg.name, sizeof(s.st.second.name));
        s.st.second.call_start_us = s.bg.start_us;
        s.st.second.line = s.bg.line;
    } else {
        memset(&s.st.second, 0, sizeof(s.st.second));
        s.st.second.call = CALL_IDLE;
    }
    st_unlock();
    update_power();

    call_event_state_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.state = state;
    ev.end_code = end_code;
    strlcpy(ev.number, number, sizeof(ev.number));
    strlcpy(ev.name, name, sizeof(ev.name));
    ESP_LOGI(TAG, "second call: %s %s (code %d)", state == CALL_ENDED ? "ended" : state == CALL_IDLE ? "to foreground"
             : call_state_name(s.bg.state), ev.number, end_code);
    esp_err_t err = esp_event_post(SIPWIRE_CALL_EVENT, CALL_EVENT_SECOND, &ev, sizeof(ev), EVENT_POST_TIMEOUT);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "post second call event: %s", esp_err_to_name(err));
    }
}

static void post_second_state(void)
{
    post_second(map_call(s.bg.state), 0, s.bg.number, s.bg.name);
}

static uint8_t other_line(void)
{
    return s.line == 1 ? 2 : 1;
}

static void fg_save(phone_line_t *l)
{
    memset(l, 0, sizeof(*l));
    l->h = s.call;
    l->state = s.state;
    l->outgoing = s.outgoing;
    l->answer_after = s.answer_after;
    l->rej = s.rej;
    l->line = s.line;
    l->media_valid = s.fg_media_valid;
    l->media = s.fg_media;
    st_lock();
    strlcpy(l->number, s.st.number, sizeof(l->number));
    strlcpy(l->name, s.st.name, sizeof(l->name));
    l->start_us = s.st.call_start_us;
    st_unlock();
}

static void fg_load(const phone_line_t *l)
{
    s.call = l->h;
    s.state = l->state;
    s.outgoing = l->outgoing;
    s.answer_after = l->answer_after;
    s.rej = l->rej;
    s.line = l->line;
    s.end_code = 0;
    s.fg_media_valid = l->media_valid;
    s.fg_media = l->media;
    s.aa_pending = false;
    st_lock();
    strlcpy(s.st.number, l->number, sizeof(s.st.number));
    strlcpy(s.st.name, l->name, sizeof(s.st.name));
    s.st.call_start_us = l->start_us;
    s.st.auto_answered = false;
    st_unlock();
}

/* Exchange foreground and background: the media stream follows the new
 * foreground call. publish: post both states now (otherwise the caller's
 * next UA call reports the new state). */
static void swap_lines(bool publish)
{
    phone_media_teardown();
    phone_dtmf_seq_clear(&s.dtmf);
    phone_line_t old;
    fg_save(&old);
    fg_load(&s.bg);
    s.bg = old;
    if (s.fg_media_valid) {
        phone_media_update(&s.fg_media);
    }
    post_second_state();
    if (publish) {
        publish_call_state();
    }
}

/* ---- transfer status ---- */

static const char *xfer_state_name(xfer_state_t x)
{
    switch (x) {
    case XFER_CONSULT:  return "consulting";
    case XFER_TRYING:   return "trying";
    case XFER_ACCEPTED: return "accepted";
    case XFER_PROGRESS: return "progress";
    case XFER_DONE:     return "done";
    case XFER_FAILED:   return "failed";
    case XFER_IDLE:
    default:            return "idle";
    }
}

static void set_xfer(xfer_state_t state, int code)
{
    call_event_transfer_t ev;
    memset(&ev, 0, sizeof(ev));
    st_lock();
    bool changed = s.st.xfer.state != state || s.st.xfer.code != code;
    s.st.xfer.state = state;
    s.st.xfer.code = code;
    ev.state = state;
    ev.code = code;
    ev.attended = s.st.xfer.attended;
    strlcpy(ev.target, s.st.xfer.target, sizeof(ev.target));
    st_unlock();
    if (state == XFER_IDLE) {
        s.xfer_h = 0;
        s.xfer_consult = 0;
    }
    if (!changed) {
        return;
    }
    ESP_LOGI(TAG, "transfer%s to %s: %s (%d)", ev.attended ? " (attended)" : "", ev.target, xfer_state_name(state),
             code);
    esp_err_t err = esp_event_post(SIPWIRE_CALL_EVENT, CALL_EVENT_TRANSFER, &ev, sizeof(ev), EVENT_POST_TIMEOUT);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "post transfer event: %s", esp_err_to_name(err));
    }
}

static void xfer_begin(bool attended, const char *target)
{
    st_lock();
    s.st.xfer.attended = attended;
    strlcpy(s.st.xfer.target, target, sizeof(s.st.xfer.target));
    st_unlock();
}

static xfer_state_t xfer_state(void)
{
    st_lock();
    xfer_state_t x = s.st.xfer.state;
    st_unlock();
    return x;
}

/* A call involved in a transfer ended (before call_ended / bg removal). */
static void xfer_call_gone(sip_call_handle_t h, int end_code)
{
    xfer_state_t x = xfer_state();
    if (h == 0 || (h != s.xfer_h && h != s.xfer_consult)) {
        return;
    }
    if (x == XFER_ACCEPTED || x == XFER_PROGRESS) {
        set_xfer(XFER_DONE, 0);         /* the far end took over and hung up */
    } else if (x == XFER_TRYING || x == XFER_CONSULT) {
        set_xfer(XFER_FAILED, end_code);
    }
    if (h == s.xfer_consult) {
        s.xfer_consult = 0;
    }
    if (h == s.xfer_h) {
        s.xfer_h = 0;
    }
}

/* The foreground call is gone: the background call (if any) moves up. */
static void promote_bg(void)
{
    phone_line_t l = s.bg;
    memset(&s.bg, 0, sizeof(s.bg));
    fg_load(&l);
    post_second(CALL_IDLE, 0, l.number, l.name);
    if (s.fg_media_valid) {
        phone_media_update(&s.fg_media);
    }
    publish_call_state();
}

/* ---- call log (directory history) ---- */

_Static_assert((int)PHONE_LOG_ANSWERED == DIR_RES_ANSWERED && (int)PHONE_LOG_MISSED == DIR_RES_MISSED &&
               (int)PHONE_LOG_REJECTED == DIR_RES_REJECTED && (int)PHONE_LOG_DND == DIR_RES_DND,
               "phone_log_result_t must match dir_result_t");

/* One history entry per ended call. Unnamed numbers are named from the contacts by
 * dir_history_add(). `start_us` != 0: the call was answered (talk time since). */
static void log_call(bool outgoing, const char *number, const char *name, int64_t start_us, bool rej, bool dnd)
{
    dir_call_t c;
    memset(&c, 0, sizeof(c));
    c.direction = outgoing ? DIR_CALL_OUT : DIR_CALL_IN;
    c.result = (uint8_t)phone_log_result(outgoing, start_us != 0, rej, dnd);
    c.duration_s = phone_log_duration_s(start_us, esp_timer_get_time());
    c.start = (time_t)phone_log_start((int64_t)time(NULL) > 1700000000 ? (int64_t)time(NULL) : 0, c.duration_s);
    strlcpy(c.number, number ? number : "", sizeof(c.number));
    strlcpy(c.name, name ? name : "", sizeof(c.name));
    if (c.number[0] == '\0') {
        return;                         /* anonymous with no number: nothing to call back */
    }
    esp_err_t err = dir_history_add(&c);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "call log: %s", esp_err_to_name(err));
    }
}

/* The call is over (UA said ENDED, or the UA went away under it). */
static void call_ended(int end_code)
{
    st_lock();
    int64_t start_us = s.st.call_start_us;
    st_unlock();
    log_call(s.outgoing, s.st.number, s.st.name, start_us, s.rej, false);   /* before promote_bg() */
    s.rej = false;
    phone_media_teardown();
    phone_dtmf_seq_clear(&s.dtmf);
    reset_call_audio_prefs();
    s.call = 0;
    s.aa_pending = false;
    s.fg_media_valid = false;
    s.state = SIP_UA_CALL_ENDED;
    s.end_code = end_code;
    s.ended_until = now_ms() + phone_ended_hold_ms(s.outgoing, end_code);
    publish_call_state();
    if (s.bg.h) {
        promote_bg();
    }
}

/* The background call is over. */
static void bg_ended(int end_code)
{
    phone_line_t l = s.bg;
    memset(&s.bg, 0, sizeof(s.bg));
    log_call(l.outgoing, l.number, l.name, l.start_us, l.rej, false);
    post_second(CALL_ENDED, end_code, l.number, l.name);
    publish_call_state();               /* stop the call-waiting beep */
}

static void go_idle(void)
{
    s.state = SIP_UA_CALL_IDLE;
    s.end_code = 0;
    s.outgoing = false;
    s.line = 0;
    st_lock();
    s.st.number[0] = '\0';
    s.st.name[0] = '\0';
    s.st.call_start_us = 0;
    s.st.auto_answered = false;
    st_unlock();
    set_xfer(XFER_IDLE, 0);
    publish_call_state();
}

/* ------------------------------------------------------------------ */
/* BLF / MWI state (sip task)                                         */
/* ------------------------------------------------------------------ */

static void post_blf(int i)
{
    blf_event_state_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.index = (uint8_t)i;
    st_lock();
    strlcpy(ev.ext, s.st.blf[i].ext, sizeof(ev.ext));
    ev.state = s.st.blf[i].state;
    st_unlock();
    esp_err_t err = esp_event_post(SIPWIRE_BLF_EVENT, BLF_EVENT_STATE, &ev, sizeof(ev), EVENT_POST_TIMEOUT);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "post blf event: %s", esp_err_to_name(err));
    }
}

static void set_blf_state(int i, blf_state_t st)
{
    st_lock();
    bool changed = s.st.blf[i].state != st;
    s.st.blf[i].state = st;
    st_unlock();
    if (changed) {
        ESP_LOGI(TAG, "blf %s%s%s%s: %s", s.blf[i].ext, s.blf[i].label[0] ? " (" : "", s.blf[i].label,
                 s.blf[i].label[0] ? ")" : "", phone_blf_state_name(st));
        post_blf(i);
    }
}

static void set_mwi(const sip_event_mwi_t *m)
{
    st_lock();
    bool changed = memcmp(&s.st.mwi, m, sizeof(*m)) != 0;
    s.st.mwi = *m;
    st_unlock();
    if (!changed) {
        return;
    }
    if (m->known) {
        ESP_LOGI(TAG, "mwi: %s, %u new / %u old voice messages", m->waiting ? "waiting" : "none",
                 (unsigned)m->new_msgs, (unsigned)m->old_msgs);
    }
    esp_err_t err = esp_event_post(SIPWIRE_SIP_EVENT, SIP_EVENT_MWI, m, sizeof(*m), EVENT_POST_TIMEOUT);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "post mwi event: %s", esp_err_to_name(err));
    }
}

static void blf_subscribe(int i)
{
    s.blf_h[i] = 0;
    if (s.ua == NULL || s.closing) {
        return;                         /* subscribed when the UA is (re)built */
    }
    sip_err_t err = sip_ua_subscribe_blf(s.ua, s.blf[i].ext, &s.blf_h[i]);
    if (err != SIP_OK) {
        s.blf_h[i] = 0;
        ESP_LOGW(TAG, "blf %s: cannot subscribe: %s", s.blf[i].ext, esp_err_to_name(map_sip_err(err)));
    }
}

/* New UA: every entry and the mailbox from scratch. */
static void subscribe_all(void)
{
    for (int i = 0; i < s.blf_n; i++) {
        blf_subscribe(i);
    }
    sip_err_t err = sip_ua_subscribe_mwi(s.ua, NULL);
    if (err != SIP_OK) {
        ESP_LOGW(TAG, "mwi: cannot subscribe: %s", esp_err_to_name(map_sip_err(err)));
    }
}

/* The UA is gone: no subscriptions, nothing known. */
static void blf_mwi_reset(void)
{
    for (int i = 0; i < s.blf_n; i++) {
        s.blf_h[i] = 0;
        set_blf_state(i, BLF_UNKNOWN);
    }
    const sip_event_mwi_t none = { 0 };
    set_mwi(&none);
}

/* Apply a (possibly) changed `blf` setting: unsubscribe removed extensions,
 * subscribe new ones, keep the rest (labels may change). */
static void blf_apply(const char *list)
{
    if (strcmp(list, s.blf_list) == 0) {
        return;
    }
    strlcpy(s.blf_list, list, sizeof(s.blf_list));
    static sipwire_blf_entry_t nw[SIPWIRE_BLF_MAX];     /* sip task only */
    int n = sipwire_blf_parse(list, nw, SIPWIRE_BLF_MAX);
    if (n < 0) {
        ESP_LOGW(TAG, "blf setting \"%s\" is invalid, no BLF keys", list);
        n = 0;
    }
    int keep[SIPWIRE_BLF_MAX];
    uint32_t drop = phone_blf_plan(s.blf, s.blf_n, nw, n, keep);
    for (int k = 0; k < s.blf_n; k++) {
        if ((drop & (1u << k)) && s.ua && s.blf_h[k]) {
            sip_ua_unsubscribe(s.ua, s.blf_h[k]);
        }
    }
    sip_sub_handle_t h[SIPWIRE_BLF_MAX];
    blf_state_t st[SIPWIRE_BLF_MAX];
    st_lock();
    for (int i = 0; i < n; i++) {
        h[i] = keep[i] >= 0 ? s.blf_h[keep[i]] : 0;
        st[i] = keep[i] >= 0 ? s.st.blf[keep[i]].state : BLF_UNKNOWN;
    }
    for (int i = 0; i < n; i++) {
        strlcpy(s.st.blf[i].ext, nw[i].ext, sizeof(s.st.blf[i].ext));
        strlcpy(s.st.blf[i].label, nw[i].label, sizeof(s.st.blf[i].label));
        s.st.blf[i].state = st[i];
        s.blf[i] = nw[i];
        s.blf_h[i] = h[i];
    }
    s.st.blf_count = (uint8_t)n;
    st_unlock();
    s.blf_n = n;
    ESP_LOGI(TAG, "blf: %d key%s", n, n == 1 ? "" : "s");
    for (int i = 0; i < n; i++) {
        if (keep[i] < 0) {
            blf_subscribe(i);
        }
        post_blf(i);
    }
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
    if (s.call != 0) {
        /* call waiting: the UA only offers it while our call is established */
        ESP_LOGI(TAG, "call waiting: %s \"%s\"", number, name);
        memset(&s.bg, 0, sizeof(s.bg));
        s.bg.h = call;
        s.bg.state = SIP_UA_CALL_IDLE;
        s.bg.answer_after = -1;
        s.bg.line = other_line();
        strlcpy(s.bg.number, number ? number : "", sizeof(s.bg.number));
        strlcpy(s.bg.name, name ? name : "", sizeof(s.bg.name));
        return;                         /* call_state(RINGING_IN) follows */
    }
    ESP_LOGI(TAG, "incoming call from %s \"%s\"", number, name);
    s.call = call;
    s.outgoing = false;
    s.end_code = 0;
    s.line = 1;
    s.rej = false;
    s.answer_after = -1;
    s.aa_pending = false;
    s.fg_media_valid = false;
    phone_dtmf_seq_clear(&s.dtmf);
    st_lock();
    strlcpy(s.st.number, number ? number : "", sizeof(s.st.number));
    strlcpy(s.st.name, name ? name : "", sizeof(s.st.name));
    s.st.call_start_us = 0;
    s.st.auto_answered = false;
    st_unlock();
    /* state follows with call_state(RINGING_IN) */
}

/* Answer the ringing foreground call as a page: loudspeaker, no ring. */
static bool auto_answer_now(void)
{
    if (s.ua == NULL || s.call == 0 || sip_call_answer(s.ua, s.call) != SIP_OK) {
        return false;
    }
    ESP_LOGI(TAG, "auto answered %s (page / intercom)", s.st.number);
    st_lock();
    s.st.auto_answered = true;
    st_unlock();
    phone_set_speaker(true);
    return true;
}

static void on_bg_state(sip_call_handle_t call, const sip_call_info_t *info)
{
    if (info->state == SIP_UA_CALL_ENDED) {
        int code = phone_end_code(info->end_reason, info->status_code);
        xfer_call_gone(call, code);
        bg_ended(code);
        return;
    }
    if (info->number && info->number[0]) {
        strlcpy(s.bg.number, info->number, sizeof(s.bg.number));
    }
    if (info->display_name && info->display_name[0]) {
        strlcpy(s.bg.name, info->display_name, sizeof(s.bg.name));
    }
    if (info->state == SIP_UA_CALL_ACTIVE && s.bg.start_us == 0) {
        s.bg.start_us = esp_timer_get_time();
    }
    if (info->state == SIP_UA_CALL_RINGING_IN) {
        s.bg.answer_after = info->answer_after;  /* a page during a call just waits */
    }
    bool changed = s.bg.state != info->state;
    s.bg.state = info->state;
    if (changed) {
        post_second_state();
        publish_call_state();           /* call-waiting beep on / off */
    }
}

static void on_call_state(void *ctx, sip_call_handle_t call, const sip_call_info_t *info)
{
    (void)ctx;
    if (s.bg.h != 0 && call == s.bg.h) {
        on_bg_state(call, info);
        return;
    }
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
        int code = phone_end_code(info->end_reason, info->status_code);
        xfer_call_gone(call, code);
        call_ended(code);
        return;
    }
    if (info->state == SIP_UA_CALL_RINGING_IN && s.state != SIP_UA_CALL_RINGING_IN) {
        s.answer_after = info->answer_after;
        if (phone_auto_answer(info->answer_after, s.f_aa, s.bg.h != 0)) {
            s.state = SIP_UA_CALL_RINGING_IN;
            if (info->answer_after == 0 && auto_answer_now()) {
                return;                 /* ACTIVE follows; never rings */
            }
            int delay = info->answer_after < AUTO_ANSWER_MAX_S ? info->answer_after : AUTO_ANSWER_MAX_S;
            s.aa_pending = true;
            s.aa_at = now_ms() + (uint64_t)delay * 1000u;
        }
    }
    s.state = info->state;
    publish_call_state();
}

static void on_xfer(void *ctx, sip_call_handle_t call, sip_xfer_state_t st, int code)
{
    (void)ctx;
    if (call == 0 || call != s.xfer_h) {
        return;
    }
    xfer_state_t x = phone_xfer_map(st);
    set_xfer(x, code);
    if (x == XFER_DONE && s.ua) {
        /* the parties are connected: leave both calls, the background one
         * first (its ENDED comes first: nothing is promoted in between) */
        sip_call_handle_t first = s.xfer_consult, second = s.xfer_h;
        if (first == 0 || s.bg.h == second) {
            first = s.xfer_h;
            second = s.xfer_consult;
        }
        if (first) {
            sip_call_hangup(s.ua, first);
        }
        if (second) {
            sip_call_hangup(s.ua, second);
        }
    } else if (x == XFER_FAILED && s.ua) {
        bool attended = false;
        st_lock();
        attended = s.st.xfer.attended;
        st_unlock();
        if (!attended && call == s.call && s.state == SIP_UA_CALL_HELD) {
            sip_call_hold(s.ua, call, false);   /* keep the call: back to talking */
        }
    }
}

static void on_rejected(void *ctx, const char *number, const char *name, int code)
{
    (void)ctx;
    call_event_missed_t ev;
    memset(&ev, 0, sizeof(ev));
    strlcpy(ev.number, number ? number : "", sizeof(ev.number));
    strlcpy(ev.name, name ? name : "", sizeof(ev.name));
    ev.code = code;
    ev.dnd = s.f_dnd;
    log_call(false, ev.number, ev.name, 0, false, ev.dnd);
    ESP_LOGI(TAG, "missed call from %s \"%s\" (%s, %d)", ev.number, ev.name, ev.dnd ? "do not disturb" : "busy",
             code);
    esp_err_t err = esp_event_post(SIPWIRE_CALL_EVENT, CALL_EVENT_MISSED, &ev, sizeof(ev), EVENT_POST_TIMEOUT);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "post missed call event: %s", esp_err_to_name(err));
    }
}

static void on_blf(void *ctx, sip_sub_handle_t sub, const char *ext, sip_blf_state_t state)
{
    (void)ctx;
    (void)ext;
    for (int i = 0; i < s.blf_n; i++) {
        if (s.blf_h[i] == sub) {
            set_blf_state(i, phone_blf_map(state));
            return;
        }
    }
}

static uint16_t cap_count(int n)
{
    return (uint16_t)(n < 0 ? 0 : n > 9999 ? 9999 : n);
}

static void on_mwi(void *ctx, bool waiting, int new_msgs, int old_msgs)
{
    (void)ctx;
    const sip_event_mwi_t m = {
        .known = true,
        .waiting = waiting,
        .new_msgs = cap_count(new_msgs),
        .old_msgs = cap_count(old_msgs),
    };
    set_mwi(&m);
}

static void on_media(void *ctx, sip_call_handle_t call, const sip_media_info_t *mi)
{
    (void)ctx;
    if (s.bg.h != 0 && call == s.bg.h) {
        s.bg.media = *mi;               /* applied when it comes to the foreground */
        s.bg.media_valid = true;
        return;
    }
    if (call != s.call || s.call == 0) {
        return;
    }
    s.fg_media = *mi;
    s.fg_media_valid = true;
    phone_media_update(mi);
    if (s.state == SIP_UA_CALL_RINGING_IN) {
        return;                 /* answering: ACTIVE follows (no media while ringing) */
    }
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
           strcmp(a->sip_display_name, b->sip_display_name) != 0 || a->sip_reg_expires != b->sip_reg_expires ||
           strcmp(a->codec_pref, b->codec_pref) != 0;
}

static void ua_destroy(void)
{
    if (s.ua == NULL) {
        return;
    }
    if (s.bg.h != 0) {
        bg_ended(0);
    }
    if (s.call != 0) {
        ESP_LOGW(TAG, "SIP stack restarted during a call");
        call_ended(0);
    }
    sip_ua_destroy(s.ua);
    s.ua = NULL;
    s.closing = false;
    sip_udp_close(&s.udp);
    blf_mwi_reset();
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

    sip_ua_config_t cfg = {
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
            .blf_state = on_blf,
            .mwi = on_mwi,
            .transfer = on_xfer,
            .call_rejected = on_rejected,
        },
    };
    cfg.n_codecs = (uint8_t)phone_codec_pref_parse(s.acc.codec_pref, cfg.codecs, SIP_UA_MAX_CODECS);
    if (cfg.n_codecs == 0) {
        if (s.acc.codec_pref[0] != '\0') {
            ESP_LOGW(TAG, "codec_pref \"%s\" invalid, using %s", s.acc.codec_pref, PHONE_CODEC_PREF_DEFAULT);
        }
        cfg.n_codecs = (uint8_t)phone_codec_pref_parse(PHONE_CODEC_PREF_DEFAULT, cfg.codecs, SIP_UA_MAX_CODECS);
    }
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
    sip_ua_set_dnd(s.ua, s.f_dnd);
    sip_ua_set_call_waiting(s.ua, s.f_cw);
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
    subscribe_all();                    /* sent once registered */
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
    if (s.bg.h != 0) {
        sip_call_hangup(s.ua, s.bg.h);  /* reports ENDED synchronously */
    }
    if (s.call != 0) {
        sip_call_hangup(s.ua, s.call);
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

/* Settings `dnd`, `call_waiting`, `auto_answer`, live. */
static void apply_features(const settings_t *st)
{
    if (st->dnd != s.f_dnd) {
        ESP_LOGI(TAG, "do not disturb %s", st->dnd ? "on" : "off");
    }
    s.f_dnd = st->dnd;
    s.f_cw = st->call_waiting;
    s.f_aa = st->auto_answer;
    st_lock();
    s.st.dnd = st->dnd;
    s.st.call_waiting = st->call_waiting;
    s.st.auto_answer = st->auto_answer;
    st_unlock();
    if (s.ua) {
        sip_ua_set_dnd(s.ua, s.f_dnd);
        sip_ua_set_call_waiting(s.ua, s.f_cw);
    }
}

static void on_settings(uint64_t now)
{
    settings_t *ns = s_cfg;
    settings_get(ns);

    st_lock();
    if (!s.vol_dirty) {
        s.ring_vol = ns->ring_volume;
        s.call_vol = ns->call_volume;
        s.spk_vol = ns->speaker_volume;
    }
    st_unlock();
    apply_volume();
    apply_aec(ns);
    apply_features(ns);

    if (account_changed(ns, &s.acc)) {
        ESP_LOGI(TAG, "SIP account changed, re-registering");
        s.acc = *ns;
        ua_restart(now);
    }
    blf_apply(ns->blf);
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
    s.rej = false;
    s.end_code = 0;
    s.line = 1;
    s.answer_after = -1;
    s.aa_pending = false;
    s.fg_media_valid = false;
    phone_dtmf_seq_clear(&s.dtmf);
    st_lock();
    s.st.auto_answered = false;
    st_unlock();
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

static bool fg_established(void)
{
    return s.call != 0 && (s.state == SIP_UA_CALL_ACTIVE || s.state == SIP_UA_CALL_HELD);
}

static sip_err_t cmd_answer_waiting(void)
{
    if (s.ua == NULL || s.bg.h == 0 || s.bg.state != SIP_UA_CALL_RINGING_IN) {
        ESP_LOGW(TAG, "answer2: no waiting call");
        return SIP_OK;
    }
    if (s.call != 0 && s.state == SIP_UA_CALL_ACTIVE) {
        sip_err_t e = sip_call_hold(s.ua, s.call, true);
        if (e != SIP_OK) {
            ESP_LOGW(TAG, "answer2: cannot hold the current call: %s", esp_err_to_name(map_sip_err(e)));
        }
    }
    swap_lines(false);                  /* no RINGING_IN blip: ACTIVE comes from the answer */
    sip_err_t err = sip_call_answer(s.ua, s.call);
    if (s.state == SIP_UA_CALL_RINGING_IN) {
        publish_call_state();           /* the answer failed: show it ringing */
    }
    return err;
}

static sip_err_t cmd_swap(void)
{
    if (s.bg.h != 0 && s.bg.state == SIP_UA_CALL_RINGING_IN) {
        return cmd_answer_waiting();
    }
    if (s.ua == NULL || s.bg.h == 0 || !fg_established() ||
        (s.bg.state != SIP_UA_CALL_HELD && s.bg.state != SIP_UA_CALL_ACTIVE)) {
        ESP_LOGW(TAG, "swap: needs two established calls");
        return SIP_OK;
    }
    sip_err_t err = sip_call_hold(s.ua, s.call, true);
    if (err == SIP_OK) {
        err = sip_call_hold(s.ua, s.bg.h, false);
    }
    swap_lines(true);
    return err;
}

static sip_err_t cmd_xfer_blind(const char *target)
{
    if (s.ua == NULL || !fg_established()) {
        ESP_LOGW(TAG, "xfer: no established call");
        return SIP_OK;
    }
    xfer_state_t x = xfer_state();
    if (x != XFER_IDLE && x != XFER_DONE && x != XFER_FAILED) {
        ESP_LOGW(TAG, "xfer: a transfer is in progress");
        return SIP_OK;
    }
    xfer_begin(false, target);
    s.xfer_h = s.call;
    s.xfer_consult = 0;
    sip_err_t err = sip_call_transfer(s.ua, s.call, target);   /* TRYING is reported inside */
    if (err != SIP_OK) {
        set_xfer(XFER_FAILED, 0);
    }
    return err;
}

static sip_err_t cmd_xfer_start(const char *target)
{
    if (s.ua == NULL || !fg_established() || s.bg.h != 0) {
        ESP_LOGW(TAG, "axfer: needs exactly one established call");
        return SIP_OK;
    }
    xfer_state_t x = xfer_state();
    if (x != XFER_IDLE && x != XFER_DONE && x != XFER_FAILED) {
        ESP_LOGW(TAG, "axfer: a transfer is in progress");
        return SIP_OK;
    }
    sip_call_handle_t orig = s.call;
    if (s.state == SIP_UA_CALL_ACTIVE) {
        sip_err_t e = sip_call_hold(s.ua, orig, true);
        if (e != SIP_OK) {
            ESP_LOGW(TAG, "axfer: cannot hold the call: %s", esp_err_to_name(map_sip_err(e)));
        }
    }
    /* the original goes to the background, the consultation call is dialled
     * in the foreground */
    phone_media_teardown();
    phone_dtmf_seq_clear(&s.dtmf);
    fg_save(&s.bg);
    uint8_t line = other_line();
    s.call = 0;
    s.outgoing = true;
    s.rej = false;
    s.end_code = 0;
    s.line = line;
    s.answer_after = -1;
    s.aa_pending = false;
    s.fg_media_valid = false;
    st_lock();
    strlcpy(s.st.number, target, sizeof(s.st.number));
    s.st.name[0] = '\0';
    s.st.call_start_us = 0;
    s.st.auto_answered = false;
    st_unlock();
    xfer_begin(true, target);
    s.xfer_h = orig;
    s.xfer_consult = 0;
    set_xfer(XFER_CONSULT, 0);
    post_second_state();

    sip_err_t err = sip_call_dial(s.ua, target, &s.call);  /* DIALING is delivered inside */
    if (err != SIP_OK) {
        s.call = 0;
        set_xfer(XFER_FAILED, 0);
        promote_bg();                   /* back to the original call, still held */
        return err;
    }
    s.xfer_consult = s.call;
    return SIP_OK;
}

static sip_err_t cmd_xfer_complete(void)
{
    if (s.ua == NULL || xfer_state() != XFER_CONSULT || s.xfer_h == 0 || s.xfer_consult == 0) {
        ESP_LOGW(TAG, "axfer complete: no consultation call");
        return SIP_OK;
    }
    sip_ua_call_state_t cs = sip_call_state(s.ua, s.xfer_consult);
    if (cs != SIP_UA_CALL_ACTIVE && cs != SIP_UA_CALL_HELD) {
        ESP_LOGW(TAG, "axfer complete: the consultation call is not answered");
        return SIP_OK;
    }
    sip_err_t err = sip_call_transfer_replace(s.ua, s.xfer_h, s.xfer_consult);
    if (err != SIP_OK) {
        set_xfer(XFER_FAILED, 0);
    }
    return err;
}

static sip_err_t cmd_xfer_cancel(void)
{
    sip_call_handle_t consult = s.xfer_consult;
    st_lock();
    bool attended = s.st.xfer.attended;
    st_unlock();
    if (s.ua == NULL || !attended || consult == 0) {
        ESP_LOGW(TAG, "axfer cancel: no consultation call");
        return SIP_OK;
    }
    set_xfer(XFER_IDLE, 0);
    return sip_call_hangup(s.ua, consult);  /* the original stays held */
}

static void handle_cmd(const phone_cmd_t *c, uint64_t now)
{
    sip_err_t err = SIP_OK;
    switch (c->type) {
    case CMD_ANSWER_WAITING:
        err = cmd_answer_waiting();
        break;
    case CMD_REJECT_WAITING:
        if (s.ua && s.bg.h && s.bg.state == SIP_UA_CALL_RINGING_IN) {
            s.bg.rej = true;
            err = sip_call_reject(s.ua, s.bg.h, WAITING_REJECT_CODE);
        } else {
            ESP_LOGW(TAG, "reject2: no waiting call");
        }
        break;
    case CMD_SWAP:
        err = cmd_swap();
        break;
    case CMD_XFER_BLIND:
        err = cmd_xfer_blind(c->arg);
        break;
    case CMD_XFER_START:
        err = cmd_xfer_start(c->arg);
        break;
    case CMD_XFER_COMPLETE:
        err = cmd_xfer_complete();
        break;
    case CMD_XFER_CANCEL:
        err = cmd_xfer_cancel();
        break;
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
            s.rej = true;
            err = sip_call_reject(s.ua, s.call, c->code);
        } else {
            ESP_LOGW(TAG, "reject: no ringing call");
        }
        break;
    case CMD_HANGUP:
        if (s.ua && s.call) {
            s.rej = s.rej || (s.state == SIP_UA_CALL_RINGING_IN && !s.outgoing);
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

    if (s.aa_pending && now >= s.aa_at) {
        s.aa_pending = false;
        if (s.state == SIP_UA_CALL_RINGING_IN && s.bg.h == 0) {
            auto_answer_now();
        }
    }

    char d = phone_dtmf_seq_due(&s.dtmf, now);
    if (d) {
        phone_dtmf_res_t r = phone_media_send_dtmf(d);
        if (r == PHONE_DTMF_BUSY) {
            phone_dtmf_seq_defer(&s.dtmf, now + DTMF_BUSY_RETRY_MS);
        } else {
            phone_dtmf_seq_consume(&s.dtmf, now);
            audio_play_dtmf(d);
            if (r == PHONE_DTMF_UNAVAILABLE) {
                /* no telephone-event negotiated: SIP INFO (dtmf-relay) */
                sip_err_t e = SIP_ERR_STATE;
                if (s.ua && s.call && s.state == SIP_UA_CALL_ACTIVE) {
                    e = sip_call_send_info_dtmf(s.ua, s.call, d, INFO_DTMF_DURATION_MS);
                }
                if (e != SIP_OK) {
                    ESP_LOGW(TAG, "dtmf %c: cannot send (no telephone-event, INFO: %s)", d,
                             esp_err_to_name(map_sip_err(e)));
                }
            }
        }
    }

    bool save = false;
    uint8_t ring = 0, call = 0, spk = 0;
    st_lock();
    if (s.vol_dirty && now >= s.vol_save_at) {
        s.vol_dirty = false;
        save = true;
        ring = s.ring_vol;
        call = s.call_vol;
        spk = s.spk_vol;
    }
    st_unlock();
    if (save) {
        settings_t *cur = s_cfg;
        settings_get(cur);
        if (cur->ring_volume != ring || cur->call_volume != call || cur->speaker_volume != spk) {
            cur->ring_volume = ring;
            cur->call_volume = call;
            cur->speaker_volume = spk;
            esp_err_t err = settings_set(cur);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "saving volume: %s", esp_err_to_name(err));
            }
        }
    }
}

/* Earliest time housekeeping() or the UA has work, UINT64_MAX = none. Mirrors the
 * conditions in housekeeping(); everything else arrives as a packet, a command or a notify,
 * and each of those ends the wait (socket / wake_fd). */
static uint64_t next_deadline(void)
{
    uint64_t d = s.ua ? sip_ua_next_deadline_ms(s.ua) : UINT64_MAX;
    if (s.closing) {
        d = phone_deadline_min(d, s.close_deadline);
    }
    if (s.need_ua && s.ua == NULL) {
        d = phone_deadline_min(d, s.retry_at);
    }
    if (s.state == SIP_UA_CALL_ENDED) {
        d = phone_deadline_min(d, s.ended_until);
    }
    if (s.aa_pending) {
        d = phone_deadline_min(d, s.aa_at);
    }
    d = phone_deadline_min(d, phone_dtmf_seq_next_ms(&s.dtmf));
    st_lock();
    if (s.vol_dirty) {
        d = phone_deadline_min(d, s.vol_save_at);
    }
    st_unlock();
    return d;
}

/* Event-driven: one pass per received datagram, command, notify or deadline instead of a
 * fixed 20 ms poll (idle: one wake-up per MAX_WAIT_MS at most, besides the SIP keepalive). */
static void sip_task(void *arg)
{
    (void)arg;
    unsigned zero_waits = 0;
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

        now = now_ms();
        uint32_t wait = phone_wait_ms(now, next_deadline(), s.wake_fd >= 0 ? MAX_WAIT_MS : POLL_MS);
        if (uxQueueMessagesWaiting(s.q) > 0) {
            wait = 0;
        }
        if (wait > 0) {
            zero_waits = 0;
        } else if (++zero_waits > SPIN_LIMIT) {
            /* a deadline that stays due without anything happening is a bug, but it must never
             * starve the lower-priority tasks on this core */
            if (zero_waits == SPIN_LIMIT + 1) {
                ESP_LOGW(TAG, "sip task: deadline stays due, throttling");
            }
            wait = SPIN_WAIT_MS;
        }

        phone_cmd_t cmd;
        bool have_cmd = false;
        if (s.ua || s.wake_fd >= 0) {
            if (sip_udp_poll_wake(&s.udp, s.ua, (int)wait, s.wake_fd) < 0) {
                vTaskDelay(pdMS_TO_TICKS(POLL_MS));     /* socket error: don't spin */
            }
        } else {
            have_cmd = xQueueReceive(s.q, &cmd, pdMS_TO_TICKS(wait)) == pdTRUE;
        }
        if (s.ua) {
            sip_ua_tick(s.ua, now_ms());
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
        notify_sip_task(NOTIFY_NET_UP);
    } else if (base == SIPWIRE_NET_EVENT && id == NET_EVENT_DISCONNECTED) {
        atomic_store(&s.net_ip, 0);     /* no new UA until the next GOT_IP */
        notify_sip_task(NOTIFY_NET_DOWN);
    } else if (base == SIPWIRE_SETTINGS_EVENT && id == SETTINGS_EVENT_CHANGED) {
        notify_sip_task(NOTIFY_SETTINGS);
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
    s_cfg = heap_caps_malloc_prefer(sizeof(*s_cfg), 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(s.st_mtx && s.q && s_cfg, ESP_ERR_NO_MEM, TAG, "no memory");
    ESP_RETURN_ON_ERROR(phone_media_init(), TAG, "media init");

    /* wake-up channel for the sip task's select(); without it the task polls every POLL_MS */
    const esp_vfs_eventfd_config_t efd_cfg = ESP_VFS_EVENTD_CONFIG_DEFAULT();
    esp_err_t efd_err = esp_vfs_eventfd_register(&efd_cfg);
    if (efd_err == ESP_OK || efd_err == ESP_ERR_INVALID_STATE) {   /* INVALID_STATE: registered */
        s.wake_fd = eventfd(0, 0);
    }
    if (s.wake_fd < 0) {
        ESP_LOGW(TAG, "no eventfd (%s): sip task polls every %d ms", esp_err_to_name(efd_err), POLL_MS);
    }

    settings_get(&s.acc);
    blf_apply(s.acc.blf);               /* the sip task does not run yet */
    s.ring_vol = s.acc.ring_volume;
    s.call_vol = s.acc.call_volume;
    s.spk_vol = s.acc.speaker_volume;
    s.st.reg = SIP_REG_NONE;
    s.st.call = CALL_IDLE;
    s.state = SIP_UA_CALL_IDLE;
    s.vstate = SIP_UA_CALL_IDLE;
    s.st.volume = s.ring_vol;
    s.st.second.call = CALL_IDLE;
    s.st.xfer.state = XFER_IDLE;
    s.answer_after = -1;
    s.f_dnd = s.acc.dnd;
    s.f_cw = s.acc.call_waiting;
    s.f_aa = s.acc.auto_answer;
    s.st.dnd = s.acc.dnd;
    s.st.call_waiting = s.acc.call_waiting;
    s.st.auto_answer = s.acc.auto_answer;
    s.need_ua = true;
    audio_set_volume(s.st.volume);
    apply_aec(&s.acc);

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
        notify_sip_task(NOTIFY_NET_UP);
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
    wake_sip_task();
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

esp_err_t phone_answer_waiting(void)
{
    const phone_cmd_t c = { .type = CMD_ANSWER_WAITING };
    return send_cmd(&c);
}

esp_err_t phone_reject_waiting(void)
{
    const phone_cmd_t c = { .type = CMD_REJECT_WAITING };
    return send_cmd(&c);
}

esp_err_t phone_swap(void)
{
    const phone_cmd_t c = { .type = CMD_SWAP };
    return send_cmd(&c);
}

static esp_err_t send_target_cmd(cmd_type_t type, const char *target)
{
    phone_cmd_t c = { .type = type };
    ESP_RETURN_ON_FALSE(target && target[0] && strlen(target) < sizeof(c.arg), ESP_ERR_INVALID_ARG, TAG,
                        "bad transfer target");
    strlcpy(c.arg, target, sizeof(c.arg));
    return send_cmd(&c);
}

esp_err_t phone_transfer_blind(const char *target)
{
    return send_target_cmd(CMD_XFER_BLIND, target);
}

esp_err_t phone_transfer_attended_start(const char *target)
{
    return send_target_cmd(CMD_XFER_START, target);
}

esp_err_t phone_transfer_attended_complete(void)
{
    const phone_cmd_t c = { .type = CMD_XFER_COMPLETE };
    return send_cmd(&c);
}

esp_err_t phone_transfer_attended_cancel(void)
{
    const phone_cmd_t c = { .type = CMD_XFER_CANCEL };
    return send_cmd(&c);
}

esp_err_t phone_set_dnd(bool on)
{
    return SETTINGS_SET(dnd, &on);      /* applied by the sip task on SETTINGS_EVENT_CHANGED */
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
    s.vol_dirty = true;                 /* all three kinds are persisted (debounced) */
    s.vol_save_at = now_ms() + VOLUME_SAVE_DELAY_MS;
    s.st.volume = volume;
    st_unlock();
    wake_sip_task();                    /* new deadline */
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
