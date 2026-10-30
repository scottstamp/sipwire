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
#include <strings.h>
#include <string.h>
#include <time.h>

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

/* Mock wall clock: the fixed date 2026-10-29 at the mock time (0 until the clock is set), so
 * history timestamps ("14:03", "Wed", "Oct 12") are the same in every screenshot. */
static time_t wall_at(int days_ago, int hour, int minute)
{
    struct tm tm;
    memset(&tm, 0, sizeof(tm));
    tm.tm_year = 2026 - 1900;
    tm.tm_mon = 9;
    tm.tm_mday = 29 - days_ago;
    tm.tm_hour = hour;
    tm.tm_min = minute;
    tm.tm_isdst = -1;
    return mktime(&tm);
}

/* ---- directory stubs ---- */

#define SIM_SPEED_SLOTS 10
static ui_contact_t s_contacts[UI_CONTACTS_MAX];   /* sorted by name; .speed unused here */
static size_t s_n_contacts;
static char s_speed[SIM_SPEED_SLOTS][UI_NUMBER_LEN];
static ui_call_t s_hist[UI_HISTORY_MAX];           /* newest first */
static size_t s_n_hist;
static uint32_t s_hist_rev;                         /* like dir_history_rev(): +1 per change */
static const char *const RINGTONES[] = {"Classic", "Digital", "Chime", "Old bell", "Soft"};
#define N_RINGTONES ((int)(sizeof(RINGTONES) / sizeof(RINGTONES[0])))

static unsigned count_unseen(void)
{
    unsigned n = 0;
    for (size_t i = 0; i < s_n_hist; i++) {
        if (s_hist[i].direction == UI_CALLDIR_IN && s_hist[i].result == UI_CALLRES_MISSED && !s_hist[i].seen) {
            n++;
        }
    }
    return n > 99 ? 99 : n;
}

static void push_model(void)
{
    s_model.missed_unseen = (uint8_t)count_unseen();
    s_model.history_rev = s_hist_rev;
    s_model.missed_last[0] = '\0';
    for (size_t i = 0; i < s_n_hist; i++) {
        const ui_call_t *c = &s_hist[i];
        if (c->direction == UI_CALLDIR_IN && c->result == UI_CALLRES_MISSED && !c->seen) {
            snprintf(s_model.missed_last, sizeof(s_model.missed_last), "%s", c->name[0] ? c->name : c->number);
            break;
        }
    }
    s_model.now = s_hour >= 0 ? (int64_t)wall_at(0, s_hour, s_minute) : 0;
    format_clock();
    ui_update(&s_model);
    js_model_changed();
}

/* ---- call features mock: second call, transfer, page ---------------------- */

static lv_timer_t *s_xfer_timer;    /* fake transfer progress */

static void clear_call_features(void)
{
    s_model.auto_answered = false;
    s_model.second_call = UI_CALL_IDLE;
    s_model.second_number[0] = '\0';
    s_model.second_name[0] = '\0';
    s_model.second_seconds = 0;
    s_model.xfer = UI_XFER_IDLE;
    s_model.xfer_attended = false;
    s_model.xfer_code = 0;
    s_model.xfer_target[0] = '\0';
}

static void cancel_xfer_timer(void)
{
    if (s_xfer_timer) {
        lv_timer_delete(s_xfer_timer);
        s_xfer_timer = NULL;
    }
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
        clear_call_features();
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

/* The foreground call goes away. Like the phone: a background call (held or waiting)
 * moves up at once instead of showing the ended screen. */
static void promote_second(void)
{
    snprintf(s_model.remote_number, sizeof(s_model.remote_number), "%s", s_model.second_number);
    snprintf(s_model.remote_name, sizeof(s_model.remote_name), "%s", s_model.second_name);
    s_model.call = s_model.second_call;
    s_model.call_seconds = s_model.second_seconds;
    s_model.second_call = UI_CALL_IDLE;
    s_model.second_number[0] = '\0';
    s_model.second_name[0] = '\0';
    s_model.second_seconds = 0;
    s_model.auto_answered = false;
    s_model.end_code = 0;
    s_model.xfer = UI_XFER_IDLE;
    cancel_xfer_timer();
    push_model();
}

static void end_call(int code)
{
    cancel_xfer_timer();
    if (s_model.second_call != UI_CALL_IDLE && s_model.xfer != UI_XFER_DONE) {
        cancel_phase();
        promote_second();
        return;
    }
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
        if (s_model.second_call == UI_CALL_HELD) {
            s_model.second_seconds++;
        }
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

/* ---- directory / sounds mock ---- */

static int find_contact(const char *number)
{
    for (size_t i = 0; i < s_n_contacts; i++) {
        if (strcmp(s_contacts[i].number, number) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static void clear_speed_for(const char *number)
{
    for (int d = UI_SPEED_FIRST; d <= UI_SPEED_LAST; d++) {
        if (strcmp(s_speed[d], number) == 0) {
            s_speed[d][0] = '\0';
        }
    }
}

static uint8_t speed_of(const char *number)
{
    for (int d = UI_SPEED_FIRST; d <= UI_SPEED_LAST; d++) {
        if (s_speed[d][0] && strcmp(s_speed[d], number) == 0) {
            return (uint8_t)d;
        }
    }
    return 0;
}

static bool contains_ci(const char *hay, const char *needle)
{
    size_t nl = strlen(needle);
    for (; *hay; hay++) {
        if (strncasecmp(hay, needle, nl) == 0) {
            return true;
        }
    }
    return nl == 0;
}

static size_t act_contacts_search(void *ctx, const char *query, ui_contact_t *out, size_t max)
{
    (void)ctx;
    size_t n = 0;
    size_t ql = strlen(query);
    for (int pass = 0; pass < 2; pass++) {   /* prefix matches first, then the rest */
        for (size_t i = 0; i < s_n_contacts && n < max; i++) {
            const ui_contact_t *c = &s_contacts[i];
            bool hit = ql == 0 || contains_ci(c->name, query) || strstr(c->number, query);
            bool prefix = ql == 0 || strncasecmp(c->name, query, ql) == 0 || strncmp(c->number, query, ql) == 0;
            if (hit && (pass == 0) == prefix) {
                out[n] = *c;
                out[n].speed = speed_of(c->number);
                n++;
            }
        }
    }
    return n;
}

static const char *act_contact_save(void *ctx, const char *old_number, const char *name, const char *number,
                                    uint8_t speed)
{
    (void)ctx;
    sim_log("action contact_save(old=%s, \"%s\", %s, speed=%u)", old_number ? old_number : "-", name, number,
            (unsigned)speed);
    if (!number[0] || !name[0]) {
        return "Name and number required";
    }
    if (old_number && old_number[0] && strcmp(old_number, number) != 0) {
        int o = find_contact(old_number);
        if (o >= 0) {
            memmove(&s_contacts[o], &s_contacts[o + 1], (s_n_contacts - (size_t)o - 1) * sizeof(s_contacts[0]));
            s_n_contacts--;
        }
        clear_speed_for(old_number);
    }
    int idx = find_contact(number);
    if (idx < 0) {
        if (s_n_contacts >= UI_CONTACTS_MAX) {
            return "Contact list is full";
        }
        size_t at = 0;
        while (at < s_n_contacts && strcasecmp(s_contacts[at].name, name) <= 0) {
            at++;
        }
        memmove(&s_contacts[at + 1], &s_contacts[at], (s_n_contacts - at) * sizeof(s_contacts[0]));
        s_n_contacts++;
        idx = (int)at;
    }
    memset(&s_contacts[idx], 0, sizeof(s_contacts[idx]));
    snprintf(s_contacts[idx].name, UI_NAME_LEN, "%s", name);
    snprintf(s_contacts[idx].number, UI_NUMBER_LEN, "%s", number);
    clear_speed_for(number);
    if (speed >= UI_SPEED_FIRST && speed <= UI_SPEED_LAST) {
        snprintf(s_speed[speed], UI_NUMBER_LEN, "%s", number);
    }
    return NULL;
}

static void act_contact_delete(void *ctx, const char *number)
{
    (void)ctx;
    sim_log("action contact_delete(%s)", number);
    int o = find_contact(number);
    if (o >= 0) {
        memmove(&s_contacts[o], &s_contacts[o + 1], (s_n_contacts - (size_t)o - 1) * sizeof(s_contacts[0]));
        s_n_contacts--;
    }
    clear_speed_for(number);
}

static bool act_speed_get(void *ctx, int digit, ui_contact_t *out)
{
    (void)ctx;
    if (digit < UI_SPEED_FIRST || digit > UI_SPEED_LAST || !s_speed[digit][0]) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    snprintf(out->number, UI_NUMBER_LEN, "%s", s_speed[digit]);
    int c = find_contact(s_speed[digit]);
    if (c >= 0) {
        snprintf(out->name, UI_NAME_LEN, "%s", s_contacts[c].name);
    }
    out->speed = (uint8_t)digit;
    return true;
}

static size_t act_history_list(void *ctx, ui_call_t *out, size_t max)
{
    (void)ctx;
    size_t n = s_n_hist < max ? s_n_hist : max;
    memcpy(out, s_hist, n * sizeof(out[0]));
    return n;
}

static void act_history_clear(void *ctx)
{
    (void)ctx;
    sim_log("action history_clear()");
    s_n_hist = 0;
    s_hist_rev++;
    push_model();
}

static void act_history_mark_seen(void *ctx)
{
    (void)ctx;
    sim_log("action history_mark_seen()");
    for (size_t i = 0; i < s_n_hist; i++) {
        s_hist[i].seen = true;
    }
    s_hist_rev++;
    push_model();
}

/* Like dir_history_add(): newest first, the oldest drops off a full list. */
static void hist_add(int dir, int res, const char *number, const char *name, uint32_t duration)
{
    if (s_n_hist == UI_HISTORY_MAX) {
        s_n_hist--;
    }
    memmove(&s_hist[1], &s_hist[0], s_n_hist * sizeof(s_hist[0]));
    s_n_hist++;
    ui_call_t *c = &s_hist[0];
    memset(c, 0, sizeof(*c));
    c->direction = (uint8_t)dir;
    c->result = (uint8_t)res;
    c->seen = !(dir == UI_CALLDIR_IN && res == UI_CALLRES_MISSED);
    c->start = s_hour >= 0 ? (int64_t)wall_at(0, s_hour, s_minute) : 0;
    c->duration_s = duration;
    snprintf(c->number, UI_NUMBER_LEN, "%s", number ? number : "");
    snprintf(c->name, UI_NAME_LEN, "%s", name ? name : "");
    s_hist_rev++;
}

static const char *act_ringtone_name(void *ctx, int idx)
{
    (void)ctx;
    return idx >= 0 && idx < N_RINGTONES ? RINGTONES[idx] : NULL;
}

static void act_ringtone_preview(void *ctx, int idx)
{
    (void)ctx;
    if (idx < 0) {
        sim_log("action ringtone_preview(stop)");
    } else {
        sim_log("action ringtone_preview(%d %s)", idx, RINGTONES[idx % N_RINGTONES]);
    }
}

static void act_save_sounds(void *ctx, uint8_t tone, uint8_t rv, uint8_t cv, uint8_t sv)
{
    (void)ctx;
    sim_log("action save_sounds(tone=%u ring=%u call=%u speaker=%u)", tone, rv, cv, sv);
    s_model.ring_tone = tone;
    s_model.ring_volume = rv;
    s_model.call_volume = cv;
    s_model.speaker_volume = sv;
    push_model();
}

static void act_save_call_features(void *ctx, bool cw, bool aa, bool dnd)
{
    (void)ctx;
    sim_log("action save_call_features(cw=%d aa=%d dnd=%d)", cw, aa, dnd);
    s_model.call_waiting = cw;
    s_model.auto_answer = aa;
    s_model.dnd = dnd;
    push_model();
}

/* Seed data (every start): 14 contacts, speed dial 2/3/5, a week of calls, all seen. */
static void seed_directory(void)
{
    static const struct { const char *name, *number; int speed; } C[] = {
        {"Alice Johnson", "1001", 0},      {"Bob Example", "1002", 2},     {"Carol Test", "1003", 0},
        {"Dave Miller", "5551234567", 0},  {"Eve Adams", "1005", 3},       {"Frank Castle", "+4915112345678", 0},
        {"Front desk", "100", 5},          {"Grace Hopper", "1007", 0},    {"Heidi Klum", "1008", 0},
        {"Ivan Petrov", "1009", 0},        {"Judy Garland", "1010", 0},    {"Karl Lagerfeld", "1011", 0},
        {"Dr. Maximilian von Hohenzollern-Sigmaringen", "1012", 0},
        {"Pizza Palace", "5557890", 0},
    };
    for (size_t i = 0; i < sizeof(C) / sizeof(C[0]); i++) {
        act_contact_save(NULL, NULL, C[i].name, C[i].number, (uint8_t)C[i].speed);
    }
    static const struct { int dir, res, days, h, m, dur; const char *name, *number; } H[] = {
        {UI_CALLDIR_IN, UI_CALLRES_MISSED, 0, 12, 20, 0, "Bob Example", "1002"},
        {UI_CALLDIR_OUT, UI_CALLRES_ANSWERED, 0, 11, 5, 185, "Alice Johnson", "1001"},
        {UI_CALLDIR_IN, UI_CALLRES_ANSWERED, 0, 9, 41, 62, "Front desk", "100"},
        {UI_CALLDIR_IN, UI_CALLRES_MISSED, 1, 17, 58, 0, "", "+4930555123456"},
        {UI_CALLDIR_OUT, UI_CALLRES_MISSED, 1, 14, 3, 0, "Carol Test", "1003"},
        {UI_CALLDIR_IN, UI_CALLRES_REJECTED, 2, 20, 12, 0, "Dave Miller", "5551234567"},
        {UI_CALLDIR_IN, UI_CALLRES_DND, 3, 22, 30, 0, "", "1099"},
        {UI_CALLDIR_OUT, UI_CALLRES_ANSWERED, 4, 8, 15, 3725, "Eve Adams", "1005"},
        {UI_CALLDIR_IN, UI_CALLRES_ANSWERED, 6, 16, 45, 7, "Dr. Maximilian von Hohenzollern-Sigmaringen", "1012"},
        {UI_CALLDIR_OUT, UI_CALLRES_ANSWERED, 12, 13, 0, 45, "", "5557890"},
    };
    s_n_hist = 0;
    for (size_t i = 0; i < sizeof(H) / sizeof(H[0]); i++) {
        ui_call_t *c = &s_hist[s_n_hist++];
        memset(c, 0, sizeof(*c));
        c->direction = (uint8_t)H[i].dir;
        c->result = (uint8_t)H[i].res;
        c->seen = true;
        c->start = (int64_t)wall_at(H[i].days, H[i].h, H[i].m);
        c->duration_s = (uint32_t)H[i].dur;
        snprintf(c->name, UI_NAME_LEN, "%s", H[i].name);
        snprintf(c->number, UI_NUMBER_LEN, "%s", H[i].number);
    }
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

/* ---- call features: waiting, swap, transfer, DND ---- */

static void swap_calls(void)
{
    char num[64], name[64];
    uint32_t secs = s_model.call_seconds;
    snprintf(num, sizeof(num), "%s", s_model.remote_number);
    snprintf(name, sizeof(name), "%s", s_model.remote_name);
    snprintf(s_model.remote_number, sizeof(s_model.remote_number), "%s", s_model.second_number);
    snprintf(s_model.remote_name, sizeof(s_model.remote_name), "%s", s_model.second_name);
    s_model.call_seconds = s_model.second_call == UI_CALL_RINGING_IN ? 0 : s_model.second_seconds;
    snprintf(s_model.second_number, sizeof(s_model.second_number), "%s", num);
    snprintf(s_model.second_name, sizeof(s_model.second_name), "%s", name);
    s_model.second_seconds = secs;
    s_model.second_call = UI_CALL_HELD;
    s_model.call = UI_CALL_ACTIVE;
    s_model.auto_answered = false;
}

static void act_answer_waiting(void *ctx)
{
    (void)ctx;
    sim_log("action answer_waiting()");
    if (s_model.second_call == UI_CALL_RINGING_IN &&
        (s_model.call == UI_CALL_ACTIVE || s_model.call == UI_CALL_HELD)) {
        swap_calls();
        push_model();
    }
}

static void act_reject_waiting(void *ctx)
{
    (void)ctx;
    sim_log("action reject_waiting()");
    if (s_model.second_call == UI_CALL_RINGING_IN) {
        s_model.second_call = UI_CALL_IDLE;
        s_model.second_number[0] = '\0';
        s_model.second_name[0] = '\0';
        push_model();
    }
}

static void act_swap(void *ctx)
{
    (void)ctx;
    sim_log("action swap()");
    if (s_model.second_call != UI_CALL_IDLE && s_model.call != UI_CALL_IDLE) {
        swap_calls();
        push_model();
    }
}

/* Fake REFER progress. Targets starting with '9' are "busy": the transfer fails with 486 and the call is kept. */
static void xfer_step_cb(lv_timer_t *t)
{
    switch (s_model.xfer) {
    case UI_XFER_TRYING:
        s_model.xfer = UI_XFER_ACCEPTED;
        break;
    case UI_XFER_ACCEPTED:
        if (s_model.xfer_target[0] == '9') {
            s_model.xfer = UI_XFER_FAILED;
            s_model.xfer_code = 486;
            lv_timer_delete(t);
            s_xfer_timer = NULL;
        } else {
            s_model.xfer = UI_XFER_PROGRESS;
        }
        break;
    case UI_XFER_PROGRESS:
        s_model.xfer = UI_XFER_DONE;
        s_model.xfer_code = 200;
        lv_timer_delete(t);
        s_xfer_timer = NULL;
        sim_log("mock: transferred, hanging up");
        s_model.second_call = UI_CALL_IDLE;
        s_model.call = UI_CALL_ENDED;
        s_model.end_code = 0;
        schedule(UI_CALL_IDLE, T_ENDED_TO_IDLE_MS);
        break;
    default:
        lv_timer_delete(t);
        s_xfer_timer = NULL;
        break;
    }
    push_model();
}

static void xfer_begin(bool attended, const char *target)
{
    cancel_xfer_timer();
    s_model.xfer_attended = attended;
    s_model.xfer_code = 0;
    snprintf(s_model.xfer_target, sizeof(s_model.xfer_target), "%s", target ? target : "");
}

static void xfer_refer(void)
{
    s_model.xfer = UI_XFER_TRYING;
    s_xfer_timer = lv_timer_create(xfer_step_cb, 900, NULL);
}

static void act_transfer_blind(void *ctx, const char *target)
{
    (void)ctx;
    sim_log("action transfer_blind(\"%s\")", target ? target : "");
    if (s_model.call != UI_CALL_ACTIVE && s_model.call != UI_CALL_HELD) {
        return;
    }
    xfer_begin(false, target);
    xfer_refer();
    push_model();
}

static void act_transfer_attended_start(void *ctx, const char *target)
{
    (void)ctx;
    sim_log("action transfer_attended_start(\"%s\")", target ? target : "");
    if (s_model.call != UI_CALL_ACTIVE || s_model.second_call != UI_CALL_IDLE) {
        return;
    }
    xfer_begin(true, target);
    /* original goes to the background (held), the consultation call is dialled */
    snprintf(s_model.second_number, sizeof(s_model.second_number), "%s", s_model.remote_number);
    snprintf(s_model.second_name, sizeof(s_model.second_name), "%s", s_model.remote_name);
    s_model.second_seconds = s_model.call_seconds;
    s_model.second_call = UI_CALL_HELD;
    snprintf(s_model.remote_number, sizeof(s_model.remote_number), "%s", s_model.xfer_target);
    s_model.remote_name[0] = '\0';
    s_model.call_seconds = 0;
    s_model.call = UI_CALL_DIALING;
    s_model.xfer = UI_XFER_CONSULT;
    schedule(UI_CALL_RINGING_OUT, T_DIAL_TO_RINGING_MS);
    push_model();
}

static void act_transfer_attended_complete(void *ctx)
{
    (void)ctx;
    sim_log("action transfer_attended_complete()");
    if (s_model.xfer != UI_XFER_CONSULT || s_model.call != UI_CALL_ACTIVE) {
        return;
    }
    xfer_refer();
    push_model();
}

static void act_transfer_attended_cancel(void *ctx)
{
    (void)ctx;
    sim_log("action transfer_attended_cancel()");
    if (s_model.xfer != UI_XFER_CONSULT) {
        return;
    }
    cancel_phase();
    s_model.xfer = UI_XFER_IDLE;
    promote_second();   /* the original stays held in the foreground */
}

static void act_set_dnd(void *ctx, bool on)
{
    (void)ctx;
    sim_log("action set_dnd(%d)", on);
    s_model.dnd = on;
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

/* Call features, for screenshots / the panel (frozen: no mock timers). `state`: UI_CALL_RINGING_IN
 * (waiting) or UI_CALL_HELD, UI_CALL_IDLE clears the background call. */
EMSCRIPTEN_KEEPALIVE void sim_set_second(int state, const char *number, const char *name, int seconds)
{
    if (state != UI_CALL_IDLE && state != UI_CALL_RINGING_IN && state != UI_CALL_HELD) {
        return;
    }
    s_model.second_call = (ui_call_state_t)state;
    snprintf(s_model.second_number, sizeof(s_model.second_number), "%s", number ? number : "");
    snprintf(s_model.second_name, sizeof(s_model.second_name), "%s", name ? name : "");
    s_model.second_seconds = seconds > 0 ? (uint32_t)seconds : 0;
    push_model();
}

/* state: ui_xfer_state_t */
EMSCRIPTEN_KEEPALIVE void sim_set_xfer(int state, int attended, int code, const char *target)
{
    if (state < UI_XFER_IDLE || state > UI_XFER_FAILED) {
        return;
    }
    cancel_xfer_timer();
    s_model.xfer = (ui_xfer_state_t)state;
    s_model.xfer_attended = attended != 0;
    s_model.xfer_code = code;
    snprintf(s_model.xfer_target, sizeof(s_model.xfer_target), "%s", target ? target : "");
    push_model();
}

EMSCRIPTEN_KEEPALIVE void sim_set_dnd(int on)
{
    s_model.dnd = on != 0;
    push_model();
}

EMSCRIPTEN_KEEPALIVE void sim_set_page(int on)
{
    s_model.auto_answered = on != 0;
    push_model();
}

/* A second incoming call while one is active (call waiting). */
EMSCRIPTEN_KEEPALIVE void sim_waiting(const char *number, const char *name)
{
    if (s_model.call != UI_CALL_ACTIVE && s_model.call != UI_CALL_HELD) {
        sim_log("mock: waiting call ignored, no call");
        return;
    }
    sim_set_second(UI_CALL_RINGING_IN, number, name, 0);
}

/* locks: in-call touch lock time (0 = off), keypad auto-lock switch, backlight going off */
EMSCRIPTEN_KEEPALIVE void sim_set_touch_lock(int seconds)
{
    s_model.call_touch_lock_s = (uint16_t)(seconds < 0 ? 0 : seconds);
    push_model();
}

EMSCRIPTEN_KEEPALIVE void sim_set_keypad_autolock(int on)
{
    s_model.keypad_autolock = on != 0;
    push_model();
}

EMSCRIPTEN_KEEPALIVE void sim_display_off(void)
{
    sim_log("mock: display off");
    ui_display_off();
    js_model_changed();
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

/* Mark the `n` newest missed incoming calls as not yet seen (home badge, bold rows in History). */
EMSCRIPTEN_KEEPALIVE void sim_set_unseen(int n)
{
    for (size_t i = 0; i < s_n_hist; i++) {
        bool missed_in = s_hist[i].direction == UI_CALLDIR_IN && s_hist[i].result == UI_CALLRES_MISSED;
        if (missed_in) {
            s_hist[i].seen = n <= 0;
            n--;
        }
    }
    s_hist_rev++;
    push_model();
}

/* Log a call now (direction 0 in / 1 out, result ui_call_result_t), like the phone at call end. */
EMSCRIPTEN_KEEPALIVE void sim_history_add(int dir, int res, const char *number, const char *name, int seconds)
{
    hist_add(dir ? UI_CALLDIR_OUT : UI_CALLDIR_IN, res < UI_CALLRES_ANSWERED || res > UI_CALLRES_DND ? 0 : res,
             number, name, seconds > 0 ? (uint32_t)seconds : 0);
    push_model();
}

/* CALL_EVENT_MISSED: an incoming call refused without ringing (dnd: do not disturb, else busy).
 * Logged like the phone does (DND -> "Do not disturb", busy -> missed) and announced. */
EMSCRIPTEN_KEEPALIVE void sim_missed(const char *number, const char *name, int dnd)
{
    hist_add(UI_CALLDIR_IN, dnd ? UI_CALLRES_DND : UI_CALLRES_MISSED, number, name, 0);
    s_model.missed_seq++;
    s_model.missed_dnd = dnd != 0;
    snprintf(s_model.missed_from, sizeof(s_model.missed_from), "%s",
             name && name[0] ? name : number && number[0] ? number : "Unknown");
    push_model();
}

/* Directory sizes for the page / tests: 0 = empty the history, 1 = empty the contacts. */
EMSCRIPTEN_KEEPALIVE void sim_clear_directory(int which)
{
    if (which == 0) {
        s_n_hist = 0;
        s_hist_rev++;
    } else {
        s_n_contacts = 0;
        memset(s_speed, 0, sizeof(s_speed));
    }
    push_model();
}

static const char *const NET_NAMES[] = {"OFF", "CONNECTING", "CONNECTED"};
static const char *const REG_NAMES[] = {"NONE", "REGISTERING", "REGISTERED", "FAILED"};
static const char *const OTA_NAMES[] = {"IDLE", "RUNNING", "DONE", "FAILED"};
static const char *const BLF_NAMES[] = {"UNKNOWN", "IDLE", "RINGING", "BUSY"};
static const char *const XFER_NAMES[] = {"IDLE", "CONSULT", "TRYING", "ACCEPTED",
                                         "PROGRESS", "DONE", "FAILED"};
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
    PUT(",\"call_touch_lock_s\":%u,\"keypad_autolock\":%s,\"keylock\":%s",
        (unsigned)m->call_touch_lock_s, m->keypad_autolock ? "true" : "false",
        ui_keylock_get() ? "true" : "false");
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
    PUT("],\"dnd\":%s,\"auto_answered\":%s,\"second_call\":\"%s\",\"second_number\":",
        m->dnd ? "true" : "false", m->auto_answered ? "true" : "false", CALL_NAMES[m->second_call]);
    PUTS(m->second_number);
    PUT(",\"second_name\":");
    PUTS(m->second_name);
    PUT(",\"second_seconds\":%u,\"xfer\":\"%s\",\"xfer_attended\":%s,\"xfer_code\":%d,\"xfer_target\":",
        (unsigned)m->second_seconds, XFER_NAMES[m->xfer], m->xfer_attended ? "true" : "false",
        m->xfer_code);
    PUTS(m->xfer_target);
    PUT(",\"missed_unseen\":%u,\"contacts\":%u,\"history\":%u,\"ring_tone\":%u,\"ring_volume\":%u,"
        "\"call_volume\":%u,\"speaker_volume\":%u,\"call_waiting\":%s,\"auto_answer\":%s}",
        (unsigned)m->missed_unseen, (unsigned)s_n_contacts, (unsigned)s_n_hist, (unsigned)m->ring_tone,
        (unsigned)m->ring_volume, (unsigned)m->call_volume, (unsigned)m->speaker_volume,
        m->call_waiting ? "true" : "false", m->auto_answer ? "true" : "false");
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
    s_model.call_touch_lock_s = 5;     /* firmware default (Kconfig SIPWIRE_CALL_TOUCH_LOCK_S) */
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
    snprintf(s_model.ota_url, sizeof(s_model.ota_url), "http://192.168.2.193/ota.bin");
    snprintf(s_model.vm_number, sizeof(s_model.vm_number), "*97");
    s_model.ringtone_count = N_RINGTONES;
    s_model.ring_tone = 1;
    s_model.ring_volume = 70;
    s_model.call_volume = 60;
    s_model.speaker_volume = 90;
    s_model.call_waiting = true;
    s_model.auto_answer = true;
    seed_directory();

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
        .ringtone_name = act_ringtone_name,
        .ringtone_preview = act_ringtone_preview,
        .save_sounds = act_save_sounds,
        .save_call_features = act_save_call_features,
    };

    ui_init(disp, &s_actions);
    ui_update(&s_model);
    lv_timer_create(call_second_cb, 1000, NULL);

    js_ready();
    js_model_changed();
    emscripten_set_main_loop(main_loop, 0, 1);
    return 0;
}
