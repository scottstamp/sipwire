/*
 * sipwire web simulator: runs components/ui (unchanged) under Emscripten +
 * SDL2, with a mock `phone` that owns the ui_model_t and implements
 * ui_actions_t. The HTML side panel (shell.html) drives it through the
 * sim_* functions below.
 */
#include <emscripten.h>
#include <emscripten/html5.h>
#include <SDL2/SDL.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "lvgl.h"
#include "ui.h"

#define SIM_W 240
#define SIM_H 320

#define T_DIAL_TO_RINGING_MS 1000
#define T_RINGING_TO_ACTIVE_MS 2000
#define T_ENDED_TO_IDLE_MS 1500
#define T_OTA_STEP_MS 120

static ui_model_t s_model;
static ui_actions_t s_actions;
static lv_timer_t *s_phase_timer;   /* one-shot call state transitions */
static char s_json[6144];
static int s_hour = -1, s_minute;   /* mock clock (local time), hour < 0 = not set */
static lv_timer_t *s_ota_timer;     /* fake update progress */
static lv_indev_t *s_mouse;         /* disabled while the calibration screen is open */
static bool s_cal_active;

/* ---- page hooks ---------------------------------------------------------- */

EM_JS(void, js_log, (const char *msg), {
    if (Module.simLog) Module.simLog(UTF8ToString(msg));
});
EM_JS(void, js_model_changed, (void), {
    if (Module.simModelChanged) Module.simModelChanged();
});
EM_JS(void, js_touch_cal, (int active), {
    if (Module.simTouchCal) Module.simTouchCal(active != 0);
});
EM_JS(void, js_ready, (void), {
    if (Module.simReady) Module.simReady();
});

static void sim_log(const char *fmt, ...)
{
    char buf[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    js_log(buf);
}

/* ---- model plumbing ------------------------------------------------------ */

/* Same text as main/ui_bridge_model.c ui_bridge_format_time (that file is device glue). */
static void format_clock(void)
{
    if (s_hour < 0) {
        s_model.time_hhmm[0] = '\0';
    } else if (s_model.clock_24h) {
        snprintf(s_model.time_hhmm, sizeof(s_model.time_hhmm), "%02d:%02d", s_hour, s_minute);
    } else {
        int h = s_hour % 12 == 0 ? 12 : s_hour % 12;
        snprintf(s_model.time_hhmm, sizeof(s_model.time_hhmm), "%d:%02d %s", h, s_minute,
                 s_hour < 12 ? "AM" : "PM");
    }
}

static void push_model(void)
{
    format_clock();
    ui_update(&s_model);
    js_model_changed();
}

static void cancel_phase(void)
{
    if (s_phase_timer) {
        lv_timer_delete(s_phase_timer);
        s_phase_timer = NULL;
    }
}

static void phase_cb(lv_timer_t *t)
{
    ui_call_state_t next = (ui_call_state_t)(uintptr_t)lv_timer_get_user_data(t);
    s_phase_timer = NULL;   /* one-shot: LVGL deletes the timer after this cb */

    switch (next) {
    case UI_CALL_RINGING_OUT:
        s_model.call = UI_CALL_RINGING_OUT;
        sim_log("mock: ringing (remote)");
        /* After ringing, the far end picks up. */
        goto schedule_active;
    case UI_CALL_ACTIVE:
        s_model.call = UI_CALL_ACTIVE;
        s_model.call_seconds = 0;
        sim_log("mock: remote answered");
        break;
    case UI_CALL_IDLE:
        s_model.call = UI_CALL_IDLE;
        s_model.remote_number[0] = '\0';
        s_model.remote_name[0] = '\0';
        s_model.call_seconds = 0;
        s_model.end_code = 0;
        s_model.muted = false;
        break;
    default:
        break;
    }
    push_model();
    return;

schedule_active:
    push_model();
    s_phase_timer = lv_timer_create(phase_cb, T_RINGING_TO_ACTIVE_MS,
                                    (void *)(uintptr_t)UI_CALL_ACTIVE);
    lv_timer_set_repeat_count(s_phase_timer, 1);
}

static void schedule(ui_call_state_t next, uint32_t ms)
{
    cancel_phase();
    s_phase_timer = lv_timer_create(phase_cb, ms, (void *)(uintptr_t)next);
    lv_timer_set_repeat_count(s_phase_timer, 1);
}

static void end_call(int code)
{
    s_model.call = UI_CALL_ENDED;
    s_model.end_code = code;
    schedule(UI_CALL_IDLE, T_ENDED_TO_IDLE_MS);
    push_model();
}

static void call_second_cb(lv_timer_t *t)
{
    (void)t;
    if (s_model.call == UI_CALL_ACTIVE || s_model.call == UI_CALL_HELD) {
        s_model.call_seconds++;
        push_model();
    }
}

/* ---- mock ui_actions_t --------------------------------------------------- */

static void act_dial(void *ctx, const char *number)
{
    (void)ctx;
    sim_log("action dial(\"%s\")", number ? number : "");
    snprintf(s_model.remote_number, sizeof(s_model.remote_number), "%s",
             number ? number : "");
    s_model.remote_name[0] = '\0';
    s_model.call_seconds = 0;
    s_model.end_code = 0;
    s_model.call = UI_CALL_DIALING;
    schedule(UI_CALL_RINGING_OUT, T_DIAL_TO_RINGING_MS);
    push_model();
}

static void act_answer(void *ctx)
{
    (void)ctx;
    sim_log("action answer()");
    if (s_model.call == UI_CALL_RINGING_IN) {
        cancel_phase();
        s_model.call = UI_CALL_ACTIVE;
        s_model.call_seconds = 0;
        push_model();
    }
}

static void act_hangup(void *ctx)
{
    (void)ctx;
    sim_log("action hangup()");
    if (s_model.call != UI_CALL_IDLE && s_model.call != UI_CALL_ENDED) {
        end_call(0);
    }
}

static void act_dtmf(void *ctx, char digit)
{
    (void)ctx;
    sim_log("action dtmf('%c')", digit);
}

static void act_set_mute(void *ctx, bool muted)
{
    (void)ctx;
    sim_log("action set_mute(%d)", muted);
    s_model.muted = muted;
    push_model();
}

static void act_set_speaker(void *ctx, bool on)
{
    (void)ctx;
    sim_log("action set_speaker(%d)", on);
    s_model.speaker = on;
    push_model();
}

static void act_set_volume(void *ctx, uint8_t volume)
{
    (void)ctx;
    sim_log("action set_volume(%u)", (unsigned)volume);
    s_model.volume = volume;
    push_model();
}

static void act_hold(void *ctx, bool hold)
{
    (void)ctx;
    sim_log("action hold(%d)", hold);
    if (hold && s_model.call == UI_CALL_ACTIVE) {
        s_model.call = UI_CALL_HELD;
    } else if (!hold && s_model.call == UI_CALL_HELD) {
        s_model.call = UI_CALL_ACTIVE;
    }
    push_model();
}

/* Like the device: saving updates what the forms prefill from; an empty
 * password keeps the stored one. */
static void act_save_wifi(void *ctx, const char *ssid, const char *pass)
{
    (void)ctx;
    sim_log("action save_wifi(ssid=\"%s\", pass=%s)", ssid ? ssid : "",
            (pass && pass[0]) ? "<set>" : "<empty>");
    snprintf(s_model.wifi_ssid, sizeof(s_model.wifi_ssid), "%s", ssid ? ssid : "");
    if (pass && pass[0]) {
        s_model.wifi_pass_set = true;
    }
    push_model();
}

static void act_save_account(void *ctx, const char *server, uint16_t port,
                             const char *user, const char *password,
                             const char *display_name)
{
    (void)ctx;
    sim_log("action save_account(server=\"%s\", port=%u, user=\"%s\", pass=%s, name=\"%s\")",
            server ? server : "", (unsigned)port, user ? user : "",
            (password && password[0]) ? "<set>" : "<empty>",
            display_name ? display_name : "");
    snprintf(s_model.sip_server, sizeof(s_model.sip_server), "%s", server ? server : "");
    s_model.sip_port = port;
    snprintf(s_model.sip_user, sizeof(s_model.sip_user), "%s", user ? user : "");
    snprintf(s_model.sip_display_name, sizeof(s_model.sip_display_name), "%s",
             display_name ? display_name : "");
    if (password && password[0]) {
        s_model.sip_pass_set = true;
    }
    push_model();
}

static void act_save_time(void *ctx, const char *ntp, const char *tz, bool clock_24h)
{
    (void)ctx;
    sim_log("action save_time(ntp=\"%s\", tz=\"%s\", clock_24h=%d)", ntp ? ntp : "", tz ? tz : "",
            clock_24h);
    snprintf(s_model.ntp_server, sizeof(s_model.ntp_server), "%s", ntp ? ntp : "");
    snprintf(s_model.tz, sizeof(s_model.tz), "%s", tz ? tz : "");
    s_model.clock_24h = clock_24h;
    push_model();
}

/* Fake update: 0 -> 100 % in ~6 s, then "Rebooting", then back to idle (the real device reboots). */
static void ota_step_cb(lv_timer_t *t)
{
    if (s_model.ota_state == UI_OTA_RUNNING) {
        if (s_model.ota_percent < 100) {
            s_model.ota_percent++;
            snprintf(s_model.ota_message, sizeof(s_model.ota_message), "Downloading %u%%",
                     (unsigned)s_model.ota_percent);
        } else {
            s_model.ota_state = UI_OTA_DONE;
            snprintf(s_model.ota_message, sizeof(s_model.ota_message), "Rebooting in 3 s...");
            lv_timer_set_period(t, 3000);
        }
    } else {
        s_model.ota_state = UI_OTA_IDLE;
        s_model.ota_percent = 0;
        s_model.ota_message[0] = '\0';
        lv_timer_delete(t);
        s_ota_timer = NULL;
    }
    push_model();
}

static void act_ota_start(void *ctx, const char *url)
{
    (void)ctx;
    sim_log("action ota_start(\"%s\")", url ? url : "");
    if (s_model.ota_state == UI_OTA_RUNNING || s_model.ota_state == UI_OTA_DONE ||
        s_model.call != UI_CALL_IDLE) {
        return;
    }
    snprintf(s_model.ota_url, sizeof(s_model.ota_url), "%s", url ? url : "");
    s_model.ota_state = UI_OTA_RUNNING;
    s_model.ota_percent = 0;
    snprintf(s_model.ota_message, sizeof(s_model.ota_message), "Connecting...");
    if (s_ota_timer) {
        lv_timer_delete(s_ota_timer);
    }
    s_ota_timer = lv_timer_create(ota_step_cb, T_OTA_STEP_MS, NULL);
    push_model();
}

/* "1002:Bob,1003" -> model tiles; kept extensions keep their state, new ones
 * come up idle (the device would show UNKNOWN until the first NOTIFY). */
static void act_save_blf(void *ctx, const char *list)
{
    (void)ctx;
    sim_log("action save_blf(\"%s\")", list ? list : "");
    ui_blf_t old[UI_BLF_MAX];
    int n_old = s_model.blf_count;
    memcpy(old, s_model.blf, sizeof(old));
    int n = 0;
    const char *p = list ? list : "";
    while (*p && n < UI_BLF_MAX) {
        const char *end = strchr(p, ',');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        char item[64];
        snprintf(item, sizeof(item), "%.*s", (int)len, p);
        p += len + (end ? 1 : 0);
        if (!item[0]) {
            continue;
        }
        char *colon = strchr(item, ':');
        if (colon) {
            *colon = '\0';
        }
        ui_blf_t *b = &s_model.blf[n++];
        snprintf(b->ext, sizeof(b->ext), "%s", item);
        snprintf(b->label, sizeof(b->label), "%s", colon ? colon + 1 : "");
        b->state = UI_BLF_IDLE;
        for (int k = 0; k < n_old; k++) {
            if (strcmp(old[k].ext, b->ext) == 0) {
                b->state = old[k].state;
            }
        }
    }
    s_model.blf_count = (uint8_t)n;
    push_model();
}

static void act_save_vm_number(void *ctx, const char *number)
{
    (void)ctx;
    sim_log("action save_vm_number(\"%s\")", number ? number : "");
    snprintf(s_model.vm_number, sizeof(s_model.vm_number), "%s", number ? number : "");
    push_model();
}

static void act_touch_cal_begin(void *ctx)
{
    (void)ctx;
    sim_log("action touch_cal_begin(): LVGL pointer off, raw samples on");
    s_cal_active = true;
    if (s_mouse) {
        lv_indev_enable(s_mouse, false);
    }
    js_touch_cal(1);
}

/* Give LVGL its mouse back only once the button is up, so the press that ended
 * calibration (Save by holding) cannot click what is underneath afterwards. */
static void mouse_back_cb(lv_timer_t *t)
{
    if (!(SDL_GetMouseState(NULL, NULL) & SDL_BUTTON_LMASK)) {
        lv_indev_enable(s_mouse, true);
        lv_timer_delete(t);
    }
}

static void act_touch_cal_end(void *ctx, const ui_touch_cal_t *cal)
{
    (void)ctx;
    s_cal_active = false;
    js_touch_cal(0);
    if (s_mouse) {
        lv_timer_create(mouse_back_cb, 50, NULL);
    }
    if (cal) {
        sim_log("action touch_cal_end(x_min=%d, x_max=%d, y_min=%d, y_max=%d)", cal->x_min,
                cal->x_max, cal->y_min, cal->y_max);
    } else {
        sim_log("action touch_cal_end(cancelled)");
    }
}

/* ---- exported to the HTML panel ------------------------------------------ */

EMSCRIPTEN_KEEPALIVE void sim_set_net(int state, int rssi)
{
    if (state < UI_NET_OFF || state > UI_NET_CONNECTED) {
        return;
    }
    s_model.net = (ui_net_state_t)state;
    s_model.rssi = (int8_t)rssi;
    if (s_model.net == UI_NET_CONNECTED) {
        snprintf(s_model.ip, sizeof(s_model.ip), "192.168.1.42");
    } else {
        s_model.ip[0] = '\0';
    }
    push_model();
}

EMSCRIPTEN_KEEPALIVE void sim_set_reg(int state)
{
    if (state < UI_REG_NONE || state > UI_REG_FAILED) {
        return;
    }
    s_model.reg = (ui_reg_state_t)state;
    if (s_model.reg == UI_REG_NONE) {
        s_model.account[0] = '\0';
    } else {
        snprintf(s_model.account, sizeof(s_model.account),
                 "Alice <alice@sip.example.com>");
    }
    push_model();
}

EMSCRIPTEN_KEEPALIVE void sim_set_battery(int pct, int charging)
{
    s_model.battery_pct = (int8_t)pct;
    s_model.charging = charging != 0;
    push_model();
}

EMSCRIPTEN_KEEPALIVE void sim_incoming(const char *number, const char *name)
{
    if (s_model.call != UI_CALL_IDLE) {
        sim_log("mock: incoming ignored, call state is %d", (int)s_model.call);
        return;
    }
    snprintf(s_model.remote_number, sizeof(s_model.remote_number), "%s",
             number ? number : "");
    snprintf(s_model.remote_name, sizeof(s_model.remote_name), "%s",
             name ? name : "");
    s_model.call_seconds = 0;
    s_model.end_code = 0;
    s_model.call = UI_CALL_RINGING_IN;
    sim_log("mock: incoming call from \"%s\" <%s>", s_model.remote_name,
            s_model.remote_number);
    push_model();
}

EMSCRIPTEN_KEEPALIVE void sim_remote_hangup(int code)
{
    if (s_model.call == UI_CALL_IDLE || s_model.call == UI_CALL_ENDED) {
        return;
    }
    sim_log("mock: remote hangup (code %d)", code);
    end_call(code);
}

/* Test/screenshot helper: jump straight to a call state without running the
 * mock's timed transitions (so the state stays put). */
EMSCRIPTEN_KEEPALIVE void sim_set_call(int state, const char *number, const char *name,
                                       int seconds)
{
    if (state < UI_CALL_IDLE || state > UI_CALL_ENDED) {
        return;
    }
    cancel_phase();
    s_model.call = (ui_call_state_t)state;
    snprintf(s_model.remote_number, sizeof(s_model.remote_number), "%s",
             number ? number : "");
    snprintf(s_model.remote_name, sizeof(s_model.remote_name), "%s", name ? name : "");
    s_model.call_seconds = seconds > 0 ? (uint32_t)seconds : 0;
    s_model.end_code = 0;
    push_model();
}

EMSCRIPTEN_KEEPALIVE void sim_key(int key, int action)
{
    if (action < UI_KEY_PRESS || action > UI_KEY_REPEAT) {
        return;
    }
    ui_key((char)key, (ui_key_action_t)action);
}

/* Raw touch sample as the device's display hook would deliver it (the page
 * calls this with a fake rotated/offset mapping while calibration is active). */
EMSCRIPTEN_KEEPALIVE void sim_touch_raw(int ax, int ay, int pressed)
{
    ui_touch_raw(ax, ay, pressed != 0);
}

/* "HH:MM" local 24 h time (what the side panel types); "" clears the clock. */
EMSCRIPTEN_KEEPALIVE void sim_set_time(const char *hhmm)
{
    int h, m;
    if (hhmm && sscanf(hhmm, "%d:%d", &h, &m) == 2 && h >= 0 && h < 24 && m >= 0 && m < 60) {
        s_hour = h;
        s_minute = m;
    } else {
        s_hour = -1;
    }
    push_model();
}

EMSCRIPTEN_KEEPALIVE void sim_set_clock24(int on)
{
    s_model.clock_24h = on != 0;
    push_model();
}

/* Jump to an update state without running the fake download (screenshots). */
EMSCRIPTEN_KEEPALIVE void sim_set_ota(int state, int percent, const char *message)
{
    if (state < UI_OTA_IDLE || state > UI_OTA_FAILED) {
        return;
    }
    if (s_ota_timer) {
        lv_timer_delete(s_ota_timer);
        s_ota_timer = NULL;
    }
    s_model.ota_state = (ui_ota_state_t)state;
    s_model.ota_percent = (uint8_t)(percent < 0 ? 0 : percent > 100 ? 100 : percent);
    snprintf(s_model.ota_message, sizeof(s_model.ota_message), "%s", message ? message : "");
    push_model();
}

/* BLF tiles for screenshots / the panel: count first, then each entry
 * (state: 0 unknown, 1 idle, 2 ringing, 3 busy). */
EMSCRIPTEN_KEEPALIVE void sim_set_blf_count(int n)
{
    s_model.blf_count = (uint8_t)(n < 0 ? 0 : n > UI_BLF_MAX ? UI_BLF_MAX : n);
    push_model();
}

EMSCRIPTEN_KEEPALIVE void sim_set_blf(int i, const char *ext, const char *label, int state)
{
    if (i < 0 || i >= UI_BLF_MAX || state < UI_BLF_UNKNOWN || state > UI_BLF_BUSY) {
        return;
    }
    ui_blf_t *b = &s_model.blf[i];
    snprintf(b->ext, sizeof(b->ext), "%s", ext ? ext : "");
    snprintf(b->label, sizeof(b->label), "%s", label ? label : "");
    b->state = (ui_blf_state_t)state;
    if (i >= s_model.blf_count) {
        s_model.blf_count = (uint8_t)(i + 1);
    }
    push_model();
}

EMSCRIPTEN_KEEPALIVE void sim_set_mwi(int waiting, int new_msgs, int old_msgs)
{
    s_model.mwi_waiting = waiting != 0;
    s_model.mwi_new = (uint16_t)(new_msgs < 0 ? 0 : new_msgs);
    s_model.mwi_old = (uint16_t)(old_msgs < 0 ? 0 : old_msgs);
    push_model();
}

static const char *const NET_NAMES[] = {"OFF", "CONNECTING", "CONNECTED"};
static const char *const REG_NAMES[] = {"NONE", "REGISTERING", "REGISTERED", "FAILED"};
static const char *const OTA_NAMES[] = {"IDLE", "RUNNING", "DONE", "FAILED"};
static const char *const BLF_NAMES[] = {"UNKNOWN", "IDLE", "RINGING", "BUSY"};
static const char *const CALL_NAMES[] = {"IDLE",        "DIALING", "RINGING_OUT", "RINGING_IN",
                                         "ACTIVE",      "HELD",    "ENDED"};

/* Append `s` as a JSON string (quotes, backslashes and control chars escaped). */
static size_t json_str(char *dst, size_t cap, size_t pos, const char *s)
{
    if (pos + 1 < cap) dst[pos++] = '"';
    for (; *s && pos + 7 < cap; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') {
            dst[pos++] = '\\';
            dst[pos++] = (char)c;
        } else if (c < 0x20) {
            pos += (size_t)snprintf(dst + pos, cap - pos, "\\u%04x", c);
        } else {
            dst[pos++] = (char)c;
        }
    }
    if (pos + 1 < cap) dst[pos++] = '"';
    dst[pos] = '\0';
    return pos;
}

EMSCRIPTEN_KEEPALIVE const char *sim_get_model_json(void)
{
    const ui_model_t *m = &s_model;
    char *o = s_json;
    const size_t cap = sizeof(s_json);
    size_t n = 0;

#define PUT(...) do { if (n < cap) n += (size_t)snprintf(o + n, cap - n, __VA_ARGS__); } while (0)
#define PUTS(s) do { if (n < cap) n = json_str(o, cap, n, (s)); } while (0)

    PUT("{\"net\":\"%s\",\"rssi\":%d,\"ip\":", NET_NAMES[m->net], m->rssi);
    PUTS(m->ip);
    PUT(",\"reg\":\"%s\",\"account\":", REG_NAMES[m->reg]);
    PUTS(m->account);
    PUT(",\"call\":\"%s\",\"remote_number\":", CALL_NAMES[m->call]);
    PUTS(m->remote_number);
    PUT(",\"remote_name\":");
    PUTS(m->remote_name);
    PUT(",\"call_seconds\":%u,\"end_code\":%d,\"muted\":%s,\"speaker\":%s,\"volume\":%u,"
        "\"battery_pct\":%d,\"charging\":%s,\"time_hhmm\":",
        (unsigned)m->call_seconds, m->end_code, m->muted ? "true" : "false",
        m->speaker ? "true" : "false", (unsigned)m->volume, m->battery_pct,
        m->charging ? "true" : "false");
    PUTS(m->time_hhmm);
    PUT(",\"wifi_ssid\":");
    PUTS(m->wifi_ssid);
    PUT(",\"wifi_pass_set\":%s,\"sip_server\":", m->wifi_pass_set ? "true" : "false");
    PUTS(m->sip_server);
    PUT(",\"sip_port\":%u,\"sip_user\":", (unsigned)m->sip_port);
    PUTS(m->sip_user);
    PUT(",\"sip_display_name\":");
    PUTS(m->sip_display_name);
    PUT(",\"sip_pass_set\":%s,\"cal_active\":%s,\"ntp_server\":", m->sip_pass_set ? "true" : "false",
        s_cal_active ? "true" : "false");
    PUTS(m->ntp_server);
    PUT(",\"tz\":");
    PUTS(m->tz);
    PUT(",\"clock_24h\":%s,\"time_raw\":", m->clock_24h ? "true" : "false");
    char raw[8] = "";
    if (s_hour >= 0) {
        snprintf(raw, sizeof(raw), "%02d:%02d", s_hour, s_minute);
    }
    PUTS(raw);
    PUT(",\"fw_version\":");
    PUTS(m->fw_version);
    PUT(",\"fw_partition\":");
    PUTS(m->fw_partition);
    PUT(",\"ota_url\":");
    PUTS(m->ota_url);
    PUT(",\"ota_state\":\"%s\",\"ota_percent\":%u,\"ota_message\":", OTA_NAMES[m->ota_state],
        (unsigned)m->ota_percent);
    PUTS(m->ota_message);
    PUT(",\"vm_number\":");
    PUTS(m->vm_number);
    PUT(",\"mwi_waiting\":%s,\"mwi_new\":%u,\"mwi_old\":%u,\"blf\":[", m->mwi_waiting ? "true" : "false",
        (unsigned)m->mwi_new, (unsigned)m->mwi_old);
    for (int i = 0; i < m->blf_count && i < UI_BLF_MAX; i++) {
        PUT("%s{\"ext\":", i ? "," : "");
        PUTS(m->blf[i].ext);
        PUT(",\"label\":");
        PUTS(m->blf[i].label);
        PUT(",\"state\":\"%s\"}", BLF_NAMES[m->blf[i].state]);
    }
    PUT("]}");
#undef PUT
#undef PUTS
    return s_json;
}

/* ---- main loop ----------------------------------------------------------- */

static uint32_t tick_cb(void)
{
    return (uint32_t)emscripten_get_now();
}

static void main_loop(void)
{
    lv_timer_handler();
}

int main(void)
{
    /* Keep SDL's keyboard handling off the page so the panel's inputs work. */
    SDL_SetHint(SDL_HINT_EMSCRIPTEN_KEYBOARD_ELEMENT, "#canvas");

    lv_init();
    lv_tick_set_cb(tick_cb);

    lv_display_t *disp = lv_sdl_window_create(SIM_W, SIM_H);
    s_mouse = lv_sdl_mouse_create();   /* mouse == touch */

    memset(&s_model, 0, sizeof(s_model));
    s_model.battery_pct = -1;
    s_model.volume = 60;
    /* plausible stored settings, so the Settings forms show up prefilled */
    snprintf(s_model.wifi_ssid, sizeof(s_model.wifi_ssid), "HomeNet-5G");
    s_model.wifi_pass_set = true;
    snprintf(s_model.sip_server, sizeof(s_model.sip_server), "sip.example.com");
    s_model.sip_port = 5060;
    snprintf(s_model.sip_user, sizeof(s_model.sip_user), "alice");
    snprintf(s_model.sip_display_name, sizeof(s_model.sip_display_name), "Alice");
    s_model.sip_pass_set = true;
    snprintf(s_model.ntp_server, sizeof(s_model.ntp_server), "pool.ntp.org");
    snprintf(s_model.tz, sizeof(s_model.tz), "UTC0");
    snprintf(s_model.fw_version, sizeof(s_model.fw_version), "v0.4.0-12-gabc1234");
    snprintf(s_model.fw_partition, sizeof(s_model.fw_partition), "ota_0");
    snprintf(s_model.ota_url, sizeof(s_model.ota_url), "http://192.168.2.154:8090/ota.bin");
    snprintf(s_model.vm_number, sizeof(s_model.vm_number), "*97");

    s_actions = (ui_actions_t){
        .ctx = NULL,
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
    };

    ui_init(disp, &s_actions);
    ui_update(&s_model);
    lv_timer_create(call_second_cb, 1000, NULL);

    js_ready();
    js_model_changed();
    emscripten_set_main_loop(main_loop, 0, 1);
    return 0;
}
