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

static ui_model_t s_model;
static ui_actions_t s_actions;
static lv_timer_t *s_phase_timer;   /* one-shot call state transitions */
static char s_json[768];

/* ---- page hooks ---------------------------------------------------------- */

EM_JS(void, js_log, (const char *msg), {
    if (Module.simLog) Module.simLog(UTF8ToString(msg));
});
EM_JS(void, js_model_changed, (void), {
    if (Module.simModelChanged) Module.simModelChanged();
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

static void push_model(void)
{
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

static void act_save_wifi(void *ctx, const char *ssid, const char *pass)
{
    (void)ctx;
    sim_log("action save_wifi(ssid=\"%s\", pass=%s)", ssid ? ssid : "",
            (pass && pass[0]) ? "<set>" : "<empty>");
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

EMSCRIPTEN_KEEPALIVE void sim_set_time(const char *hhmm)
{
    snprintf(s_model.time_hhmm, sizeof(s_model.time_hhmm), "%s",
             hhmm ? hhmm : "");
    push_model();
}

static const char *const NET_NAMES[] = {"OFF", "CONNECTING", "CONNECTED"};
static const char *const REG_NAMES[] = {"NONE", "REGISTERING", "REGISTERED", "FAILED"};
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
    PUT("}");
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
    lv_sdl_mouse_create();   /* mouse == touch */

    memset(&s_model, 0, sizeof(s_model));
    s_model.battery_pct = -1;
    s_model.volume = 60;

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
    };

    ui_init(disp, &s_actions);
    ui_update(&s_model);
    lv_timer_create(call_second_cb, 1000, NULL);

    js_ready();
    js_model_changed();
    emscripten_set_main_loop(main_loop, 0, 1);
    return 0;
}
