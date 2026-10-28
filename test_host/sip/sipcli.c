/* sipcli: host command-line SIP client built from the real UA + sip_udp.c.
 * Used by integration.sh against a real Asterisk. Runs a script of steps
 * given as arguments and exits non-zero on the first failing step.
 *
 *   sipcli [--server ip:port] [--user u] [--pass p] [--local-ip ip]
 *          [--port n] [--rtp n] [-v] STEP...
 *
 * Steps:
 *   register | unregister          wait for REGISTERED / NONE (10 s)
 *   dial:<number>                  start a call
 *   wait:<state>[:<sec>]           dialing|ringing_out|ringing_in|active|held|ended
 *   incoming[:<sec>]               wait for an incoming call
 *   answer | hangup | hold | unhold
 *   sleep:<sec>                    keep the UA running
 *   expect-media                   media reported: port 12000-12100, PT 0/8
 *   expect-end:<reason>            local|remote|rejected|cancelled|timeout|error
 *   expect-log:<text>              a log line contained <text>
 *   shell:<cmd>                    run cmd via /bin/sh, must exit 0
 *   echo:<text>                    print a scenario header
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "sip_ua.h"

#define MAX_LOG 512

static sip_ua_t *ua;
static sip_udp_t udp;
static bool verbose;

static sip_ua_reg_state_t reg_state = SIP_UA_REG_NONE;
static sip_call_handle_t cur_call;
static sip_ua_call_state_t call_state = SIP_UA_CALL_IDLE;
static sip_ua_end_reason_t end_reason;
static bool have_media;
static sip_media_info_t media;
static bool got_incoming;
static char *logs[MAX_LOG];
static int n_logs;

static const char *state_names[] = { "idle", "dialing", "ringing_out", "ringing_in", "active", "held", "ended" };
static const char *reason_names[] = { "none", "local", "remote", "rejected", "cancelled", "timeout", "error" };
static const char *reg_names[] = { "NONE", "REGISTERING", "REGISTERED", "FAILED" };
static const char *dir_names[] = { "sendrecv", "sendonly", "recvonly", "inactive" };

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static uint32_t rand_cb(void *ctx)
{
    (void)ctx;
    static uint32_t s;
    if (!s) {
        s = (uint32_t)now_ms() ^ 0x9e3779b9u;
    }
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}

static void log_cb(void *ctx, sip_log_level_t lvl, const char *msg)
{
    (void)ctx;
    static const char lv[] = "?EWIDV";
    if (lvl <= SIP_LOG_INFO || verbose) {
        printf("LOG [%c] %s\n", lv[lvl], msg);
    }
    if (n_logs < MAX_LOG) {
        logs[n_logs++] = strdup(msg);
    }
}

static void reg_cb(void *ctx, sip_ua_reg_state_t st, int code)
{
    (void)ctx;
    reg_state = st;
    printf("EVENT reg %s %d\n", reg_names[st], code);
}

static void incoming_cb(void *ctx, sip_call_handle_t call, const char *number, const char *name)
{
    (void)ctx;
    printf("EVENT incoming call=%d from=%s name=\"%s\"\n", call, number, name);
    cur_call = call;
    got_incoming = true;
}

static void call_cb(void *ctx, sip_call_handle_t call, const sip_call_info_t *info)
{
    (void)ctx;
    printf("EVENT call=%d state=%s code=%d reason=%s\n", call, state_names[info->state],
           info->status_code, reason_names[info->end_reason]);
    if (call == cur_call) {
        call_state = info->state;
        end_reason = info->end_reason;
    }
}

static void media_cb(void *ctx, sip_call_handle_t call, const sip_media_info_t *m)
{
    (void)ctx;
    printf("EVENT media call=%d remote=%s:%u pt=%u dtmf=%d ptime=%u dir=%s local=%u%s\n", call,
           m->remote_ip_str, (unsigned)m->remote_port, (unsigned)m->payload_type, (int)m->dtmf_pt,
           (unsigned)m->ptime_ms, dir_names[m->direction], (unsigned)m->local_port, m->early ? " early" : "");
    media = *m;
    have_media = true;
}

static void run_for(uint64_t ms)
{
    uint64_t end = now_ms() + ms;
    do {
        sip_udp_poll(&udp, ua, 20);
        sip_ua_tick(ua, now_ms());
    } while (now_ms() < end);
}

static bool run_until(bool (*cond)(const void *), const void *arg, uint64_t ms)
{
    uint64_t end = now_ms() + ms;
    while (now_ms() < end) {
        sip_udp_poll(&udp, ua, 20);
        sip_ua_tick(ua, now_ms());
        if (cond(arg)) {
            return true;
        }
    }
    return false;
}

static bool reg_is(const void *a)
{
    return reg_state == *(const sip_ua_reg_state_t *)a;
}

static bool call_is(const void *a)
{
    return call_state == *(const sip_ua_call_state_t *)a;
}

static bool incoming_seen(const void *a)
{
    (void)a;
    return got_incoming;
}

static int parse_secs(const char *s, int def)
{
    return s && *s ? atoi(s) : def;
}

static bool step(const char *st)
{
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "%s", st);
    char *arg = strchr(cmd, ':');
    if (arg) {
        *arg++ = '\0';
    }
    if (!strcmp(cmd, "echo")) {
        printf("=== %s\n", arg ? arg : "");
        return true;
    }
    printf("STEP %s\n", st);
    fflush(stdout);
    if (!strcmp(cmd, "register")) {
        sip_ua_reg_state_t want = SIP_UA_REG_REGISTERED;
        sip_ua_register(ua);
        return run_until(reg_is, &want, 10000);
    }
    if (!strcmp(cmd, "unregister")) {
        sip_ua_reg_state_t want = SIP_UA_REG_NONE;
        sip_ua_unregister(ua);
        return run_until(reg_is, &want, 10000);
    }
    if (!strcmp(cmd, "dial") && arg) {
        have_media = false;
        call_state = SIP_UA_CALL_IDLE;
        return sip_call_dial(ua, arg, &cur_call) == SIP_OK;
    }
    if (!strcmp(cmd, "wait") && arg) {
        char *secs = strchr(arg, ':');
        if (secs) {
            *secs++ = '\0';
        }
        for (int i = 0; i < 7; i++) {
            if (!strcmp(arg, state_names[i])) {
                sip_ua_call_state_t want = (sip_ua_call_state_t)i;
                return call_is(&want) || run_until(call_is, &want, (uint64_t)parse_secs(secs, 10) * 1000u);
            }
        }
        return false;
    }
    if (!strcmp(cmd, "incoming")) {
        got_incoming = false;
        have_media = false;
        return run_until(incoming_seen, NULL, (uint64_t)parse_secs(arg, 10) * 1000u);
    }
    if (!strcmp(cmd, "answer")) {
        return sip_call_answer(ua, cur_call) == SIP_OK;
    }
    if (!strcmp(cmd, "hangup")) {
        return sip_call_hangup(ua, cur_call) == SIP_OK;
    }
    if (!strcmp(cmd, "hold") || !strcmp(cmd, "unhold")) {
        bool hold = !strcmp(cmd, "hold");
        sip_ua_call_state_t want = hold ? SIP_UA_CALL_HELD : SIP_UA_CALL_ACTIVE;
        return sip_call_hold(ua, cur_call, hold) == SIP_OK && run_until(call_is, &want, 5000);
    }
    if (!strcmp(cmd, "sleep")) {
        run_for((uint64_t)parse_secs(arg, 1) * 1000u);
        return true;
    }
    if (!strcmp(cmd, "expect-media")) {
        bool ok = have_media && media.remote_port >= 12000 && media.remote_port <= 12100 &&
                  (media.payload_type == 0 || media.payload_type == 8);
        if (!ok) {
            printf("FAIL media: have=%d port=%u pt=%u\n", have_media, (unsigned)media.remote_port,
                   (unsigned)media.payload_type);
        }
        return ok;
    }
    if (!strcmp(cmd, "expect-end") && arg) {
        bool ok = call_state == SIP_UA_CALL_ENDED && !strcmp(reason_names[end_reason], arg);
        if (!ok) {
            printf("FAIL end: state=%s reason=%s\n", state_names[call_state], reason_names[end_reason]);
        }
        return ok;
    }
    if (!strcmp(cmd, "expect-log") && arg) {
        for (int i = 0; i < n_logs; i++) {
            if (strstr(logs[i], arg)) {
                return true;
            }
        }
        printf("FAIL: no log line contains \"%s\"\n", arg);
        return false;
    }
    if (!strcmp(cmd, "shell") && arg) {
        int rc = system(st + 6);
        return rc == 0;
    }
    printf("unknown step \"%s\"\n", st);
    return false;
}

int main(int argc, char **argv)
{
    const char *server = "127.0.0.1:5080";
    const char *user = "1001";
    const char *pass = "secret1001";
    const char *local_ip = NULL;
    int port = 0, rtp = 40000;
    int i = 1;
    for (; i < argc && argv[i][0] == '-'; i++) {
        if (!strcmp(argv[i], "-v")) {
            verbose = true;
        } else if (i + 1 < argc && !strcmp(argv[i], "--server")) {
            server = argv[++i];
        } else if (i + 1 < argc && !strcmp(argv[i], "--user")) {
            user = argv[++i];
        } else if (i + 1 < argc && !strcmp(argv[i], "--pass")) {
            pass = argv[++i];
        } else if (i + 1 < argc && !strcmp(argv[i], "--local-ip")) {
            local_ip = argv[++i];
        } else if (i + 1 < argc && !strcmp(argv[i], "--port")) {
            port = atoi(argv[++i]);
        } else if (i + 1 < argc && !strcmp(argv[i], "--rtp")) {
            rtp = atoi(argv[++i]);
        } else {
            fprintf(stderr, "bad option %s\n", argv[i]);
            return 2;
        }
    }
    setvbuf(stdout, NULL, _IOLBF, 0);

    char host[64];
    snprintf(host, sizeof(host), "%s", server);
    char *colon = strchr(host, ':');
    uint16_t sport = 5060;
    if (colon) {
        *colon = '\0';
        sport = (uint16_t)atoi(colon + 1);
    }
    sip_addr_t srv;
    if (sip_udp_resolve(host, sport, &srv) != SIP_OK) {
        fprintf(stderr, "cannot resolve %s\n", host);
        return 2;
    }
    if (sip_udp_open(&udp, (uint16_t)port) != SIP_OK) {
        fprintf(stderr, "cannot bind UDP\n");
        return 2;
    }
    uint32_t lip;
    if (local_ip ? !sip_ip_parse(local_ip, &lip) : sip_udp_local_ip_for(&srv, &lip) != SIP_OK) {
        fprintf(stderr, "cannot determine local IP\n");
        return 2;
    }
    sip_ua_config_t cfg = {
        .server_host = host,
        .server_port = sport,
        .server_addr = srv,
        .user = user,
        .password = pass,
        .display_name = "sipcli",
        .expires = 120,
        .local_addr = { lip, udp.port },
        .rtp_port = (uint16_t)rtp,
        .keepalive_s = 30,
        .send = sip_udp_send,
        .send_ctx = &udp,
        .rand = rand_cb,
        .log = log_cb,
        .log_level = SIP_LOG_VERBOSE,
        .cb = { reg_cb, incoming_cb, call_cb, media_cb, NULL },
    };
    if (sip_ua_create(&cfg, &ua) != SIP_OK) {
        fprintf(stderr, "bad config\n");
        return 2;
    }
    sip_ua_tick(ua, now_ms());
    int rc = 0;
    for (; i < argc; i++) {
        if (!step(argv[i])) {
            printf("RESULT FAIL at step \"%s\"\n", argv[i]);
            rc = 1;
            break;
        }
    }
    if (rc == 0) {
        printf("RESULT OK\n");
    }
    run_for(200);
    sip_ua_destroy(ua);
    sip_udp_close(&udp);
    for (int k = 0; k < n_logs; k++) {
        free(logs[k]);
    }
    return rc;
}
