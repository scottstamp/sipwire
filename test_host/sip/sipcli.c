/* sipcli: host command-line SIP client built from the real UA + sip_udp.c.
 * Used by integration.sh against a real Asterisk. Runs a script of steps
 * given as arguments and exits non-zero on the first failing step.
 *
 *   sipcli [--server ip:port] [--user u] [--pass p] [--local-ip ip]
 *          [--port n] [--rtp n] [--codecs 9,0,8] [-v] STEP...
 *
 * Steps:
 *   register | unregister          wait for REGISTERED / NONE (10 s)
 *   dial:<number>                  start a call
 *   wait:<state>[:<sec>]           dialing|ringing_out|ringing_in|active|held|ended
 *   incoming[:<sec>]               wait for an incoming call
 *   answer | hangup | hold | unhold
 *   sleep:<sec>                    keep the UA running
 *   expect-media[:<pt>]            media reported: port 12000-12100, PT 9/0/8
 *                                  (or exactly <pt>)
 *   tone:<hz>:<sec>                send a <hz> sine as RTP (negotiated codec,
 *                                  16 kHz source) for <sec> s while playing
 *                                  out what comes back; with 600 (Echo) the
 *                                  tone must return at the same frequency and
 *                                  level (G.722 carries 5 kHz, G.711 cannot)
 *   send-tone:<hz>:<sec>           the same without checking what comes back
 *   expect-end:<reason>            local|remote|rejected|cancelled|timeout|error
 *   expect-log:<text>              a log line contained <text>
 *   shell:<cmd>                    run cmd via /bin/sh, must exit 0
 *   echo:<text>                    print a scenario header
 *   subscribe-blf:<ext>            BLF ("dialog") subscription to <ext>
 *   unsubscribe-blf:<ext>
 *   expect-blf:<ext>:<state>[:<sec>]  wait (10 s) until <ext> reports
 *                                  unknown|idle|ringing|busy
 *   subscribe-mwi                  "message-summary" subscription (our AOR)
 *   expect-mwi:<yes|no>[:<min_new>[:<sec>]]  wait (10 s) for that MWI state
 *                                  (subscription or unsolicited NOTIFY)
 *   dnd:<on|off>                   sip_ua_set_dnd
 *   alert-marker:<m>               sip_ua_set_alert_marker; a matching call rings
 *                                  even with DND on (screen RING, as the phone does)
 *   expect-alert                   the last incoming call carried the marker
 *   forward:<target>               screen every new INVITE to a 302 to <target>
 *   forward-off
 *   redirect:<target>              302 the current ringing call (no answer)
 *   expect-rejected:<code>         the last refused / redirected INVITE got <code>
 */

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include "rtp.h"
#include "sip_ua.h"

#define MAX_LOG 20000

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
static bool last_alert;         /* last RINGING_IN carried the alert marker */
static bool find_ring;          /* alert-marker set: ring matching calls through DND */
static char fwd_target[64];     /* forward: every new INVITE gets a 302 here */
static int last_rejected;       /* code of the last call_rejected report, 0 = none */
static char *logs[MAX_LOG];
static int n_logs;

#define MAX_BLF 8
static struct {
    char ext[32];
    sip_sub_handle_t h;
    sip_blf_state_t st;
} blf[MAX_BLF];
static int n_blf;
static bool mwi_seen, mwi_waiting;
static int mwi_new, mwi_old;
static const char *blf_names[] = { "unknown", "idle", "ringing", "busy" };

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
        /* several sipcli run side by side (integration.sh): mix in the pid */
        s = ((uint32_t)now_ms() ^ 0x9e3779b9u) + (uint32_t)getpid() * 2654435761u;
        if (!s) {
            s = 1;
        }
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
    if (info->state == SIP_UA_CALL_RINGING_IN) {
        last_alert = info->alert_match;
        if (last_alert) {
            printf("EVENT call=%d alert marker\n", call);
        }
    }
}

static void rejected_cb(void *ctx, const char *number, const char *name, int code)
{
    (void)ctx;
    printf("EVENT rejected from=%s name=\"%s\" code=%d\n", number, name, code);
    last_rejected = code;
}

static void screen_cb(void *ctx, const sip_screen_info_t *in, sip_screen_t *out)
{
    (void)ctx;
    if (fwd_target[0]) {
        out->action = SIP_SCREEN_REDIRECT;
        snprintf(out->target, sizeof(out->target), "%s", fwd_target);
    } else if (find_ring && in->alert_match) {
        out->action = SIP_SCREEN_RING;
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

static void blf_cb(void *ctx, sip_sub_handle_t sub, const char *ext, sip_blf_state_t st)
{
    (void)ctx;
    printf("EVENT blf %s %s\n", ext, blf_names[st]);
    for (int i = 0; i < n_blf; i++) {
        if (blf[i].h == sub) {
            blf[i].st = st;
        }
    }
}

static void mwi_cb(void *ctx, bool waiting, int new_msgs, int old_msgs)
{
    (void)ctx;
    printf("EVENT mwi %s new=%d old=%d\n", waiting ? "yes" : "no", new_msgs, old_msgs);
    mwi_seen = true;
    mwi_waiting = waiting;
    mwi_new = new_msgs;
    mwi_old = old_msgs;
}

static const char *xfer_names[] = { "trying", "accepted", "progress", "success", "failed" };
static int xfer_state = -1;     /* last sip_xfer_state_t, -1 = none */

static void xfer_cb(void *ctx, sip_call_handle_t call, sip_xfer_state_t st, int code)
{
    (void)ctx;
    printf("EVENT transfer call=%d %s code=%d\n", call, xfer_names[st], code);
    xfer_state = (int)st;
}

static bool xfer_is(const void *a)
{
    return xfer_state == *(const int *)a;
}

typedef struct {
    int idx;
    sip_blf_state_t st;
} blf_want_t;

static bool blf_is(const void *a)
{
    const blf_want_t *w = a;
    return blf[w->idx].st == w->st;
}

typedef struct {
    bool waiting;
    int min_new;
} mwi_want_t;

static bool mwi_is(const void *a)
{
    const mwi_want_t *w = a;
    return mwi_seen && mwi_waiting == w->waiting && mwi_new >= w->min_new;
}

static int blf_find(const char *ext)
{
    for (int i = 0; i < n_blf; i++) {
        if (!strcmp(blf[i].ext, ext)) {
            return i;
        }
    }
    return -1;
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

/* ---- RTP tone check -------------------------------------------------- */

static double goertzel(const int16_t *x, size_t n, double f)
{
    double w = 2.0 * M_PI * f / RTP_PCM_RATE, c = 2.0 * cos(w), s1 = 0, s2 = 0;
    for (size_t i = 0; i < n; i++) {
        double s0 = x[i] + c * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    return (s1 * s1 + s2 * s2 - c * s1 * s2) / ((double)n * (double)n);
}

/* Send a tone through the negotiated codec and play out what comes back.
 * check: the last second played must be the tone (>= 90 % of the energy at
 * hz) within 3 dB of the level sent. */
static bool tone_run(double hz, int secs, bool check)
{
    const double amp = 8000;
    if (!have_media || media.remote_port == 0 || secs < 2) {
        printf("FAIL tone: no media\n");
        return false;
    }
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in local = { .sin_family = AF_INET, .sin_port = htons(media.local_port) };
    if (fd < 0 || bind(fd, (struct sockaddr *)&local, sizeof(local)) != 0) {
        printf("FAIL tone: bind %u: %s\n", (unsigned)media.local_port, strerror(errno));
        if (fd >= 0) {
            close(fd);
        }
        return false;
    }
    struct timeval tv = { 0, 1000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    rtp_config_t rc = {
        .payload_type = media.payload_type,
        .dtmf_payload_type = media.dtmf_pt > 0 ? (uint8_t)media.dtmf_pt : 0,
        .ssrc = rand_cb(NULL),
        .initial_seq = (uint16_t)rand_cb(NULL),
        .initial_ts = rand_cb(NULL),
    };
    rtp_session_t *rs = rtp_session_create(&rc);
    struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons(media.remote_port),
                              .sin_addr.s_addr = htonl(media.remote_ip) };
    size_t frames = (size_t)secs * 1000 / RTP_FRAME_MS;
    int16_t *rec = calloc(frames * RTP_FRAME_SAMPLES, sizeof(int16_t));
    if (rs == NULL || rec == NULL) {
        printf("FAIL tone: alloc\n");
        close(fd);
        rtp_session_destroy(rs);
        free(rec);
        return false;
    }

    uint64_t t0 = now_ms();
    uint32_t rx = 0;
    for (size_t k = 0; k < frames; k++) {
        int16_t pcm[RTP_FRAME_SAMPLES];
        for (int i = 0; i < RTP_FRAME_SAMPLES; i++) {
            double n = (double)(k * RTP_FRAME_SAMPLES + (size_t)i);
            pcm[i] = (int16_t)lrint(amp * sin(2.0 * M_PI * hz * n / RTP_PCM_RATE));
        }
        uint8_t pkt[RTP_MAX_PACKET];
        size_t len = rtp_build_audio(rs, pcm, pkt, sizeof(pkt));
        sendto(fd, pkt, len, 0, (struct sockaddr *)&to, sizeof(to));
        /* until the next 20 ms tick: receive RTP, keep SIP alive */
        uint64_t due = t0 + (k + 1) * RTP_FRAME_MS;
        while (now_ms() < due) {
            uint8_t buf[1500];
            ssize_t n = recv(fd, buf, sizeof(buf), 0);
            if (n > 0) {
                rtp_on_packet(rs, buf, (size_t)n, (uint32_t)now_ms());
                rx++;
            }
            sip_udp_poll(&udp, ua, 0);
            sip_ua_tick(ua, now_ms());
        }
        rtp_pull_frame(rs, rec + k * RTP_FRAME_SAMPLES, (uint32_t)now_ms());
    }

    /* analyse the last second */
    size_t n = RTP_PCM_RATE;
    const int16_t *x = rec + frames * RTP_FRAME_SAMPLES - n;
    double ms = 0;
    for (size_t i = 0; i < n; i++) {
        ms += (double)x[i] * x[i];
    }
    ms /= (double)n;
    double p = goertzel(x, n, hz);
    double level_db = 20.0 * log10((2.0 * sqrt(p) + 1e-9) / amp);
    double purity = ms > 0 ? 2.0 * p / ms : 0;
    rtp_stats_t st;
    rtp_get_stats(rs, &st);
    if (check) {
        printf("TONE %.0f Hz pt %u: rx %u pkts (lost %u, plc %u), level %+.1f dB, %.1f %% of energy at %.0f Hz\n",
               hz, (unsigned)media.payload_type, (unsigned)rx, (unsigned)st.rx_lost, (unsigned)st.plc_frames,
               level_db, 100.0 * purity, hz);
    } else {
        printf("TONE %.0f Hz pt %u: sent %u frames\n", hz, (unsigned)media.payload_type, (unsigned)frames);
    }
    rtp_session_destroy(rs);
    free(rec);
    close(fd);
    bool ok = !check || (rx > frames / 2 && purity >= 0.9 && fabs(level_db) <= 3.0);
    if (!ok) {
        printf("FAIL tone: echoed %.0f Hz tone missing or distorted\n", hz);
    }
    return ok;
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
    if (!strcmp(cmd, "xfer") && arg) {
        xfer_state = -1;
        return sip_call_transfer(ua, cur_call, arg) == SIP_OK;
    }
    if (!strcmp(cmd, "expect-xfer") && arg) {
        char *secs = strchr(arg, ':');
        if (secs) {
            *secs++ = '\0';
        }
        for (int i = 0; i < 5; i++) {
            if (!strcmp(arg, xfer_names[i])) {
                return xfer_is(&i) || run_until(xfer_is, &i, (uint64_t)parse_secs(secs, 10) * 1000u);
            }
        }
        return false;
    }
    if (!strcmp(cmd, "sleep")) {
        run_for((uint64_t)parse_secs(arg, 1) * 1000u);
        return true;
    }
    if ((!strcmp(cmd, "tone") || !strcmp(cmd, "send-tone")) && arg) {
        char *secs = strchr(arg, ':');
        return secs && tone_run(atof(arg), atoi(secs + 1), !strcmp(cmd, "tone"));
    }
    if (!strcmp(cmd, "expect-media")) {
        bool pt_ok = arg ? media.payload_type == atoi(arg)
                         : (media.payload_type == 9 || media.payload_type == 0 || media.payload_type == 8);
        bool ok = have_media && media.remote_port >= 12000 && media.remote_port <= 12100 && pt_ok;
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
    if (!strcmp(cmd, "subscribe-blf") && arg) {
        if (n_blf == MAX_BLF || strlen(arg) >= sizeof(blf[0].ext)) {
            return false;
        }
        snprintf(blf[n_blf].ext, sizeof(blf[n_blf].ext), "%s", arg);
        blf[n_blf].st = SIP_BLF_UNKNOWN;
        return sip_ua_subscribe_blf(ua, arg, &blf[n_blf++].h) == SIP_OK;
    }
    if (!strcmp(cmd, "unsubscribe-blf") && arg) {
        int i = blf_find(arg);
        return i >= 0 && sip_ua_unsubscribe(ua, blf[i].h) == SIP_OK;
    }
    if (!strcmp(cmd, "expect-blf") && arg) {
        char *st_name = strchr(arg, ':');
        if (!st_name) {
            return false;
        }
        *st_name++ = '\0';
        char *secs = strchr(st_name, ':');
        if (secs) {
            *secs++ = '\0';
        }
        blf_want_t w = { blf_find(arg), SIP_BLF_UNKNOWN };
        bool known = false;
        for (int i = 0; i < 4; i++) {
            if (!strcmp(st_name, blf_names[i])) {
                w.st = (sip_blf_state_t)i;
                known = true;
            }
        }
        if (w.idx < 0 || !known) {
            return false;
        }
        bool ok = blf_is(&w) || run_until(blf_is, &w, (uint64_t)parse_secs(secs, 10) * 1000u);
        if (!ok) {
            printf("FAIL blf %s: is %s, want %s\n", arg, blf_names[blf[w.idx].st], st_name);
        }
        return ok;
    }
    if (!strcmp(cmd, "subscribe-mwi")) {
        return sip_ua_subscribe_mwi(ua, NULL) == SIP_OK;
    }
    if (!strcmp(cmd, "expect-mwi") && arg) {
        char *rest = strchr(arg, ':');
        if (rest) {
            *rest++ = '\0';
        }
        char *secs = rest ? strchr(rest, ':') : NULL;
        if (secs) {
            *secs++ = '\0';
        }
        mwi_want_t w = { !strcmp(arg, "yes"), rest ? atoi(rest) : 0 };
        bool ok = mwi_is(&w) || run_until(mwi_is, &w, (uint64_t)parse_secs(secs, 10) * 1000u);
        if (!ok) {
            printf("FAIL mwi: seen=%d waiting=%d new=%d old=%d\n", mwi_seen, mwi_waiting, mwi_new, mwi_old);
        }
        return ok;
    }
    if (!strcmp(cmd, "dnd") && arg) {
        sip_ua_set_dnd(ua, !strcmp(arg, "on"));
        return true;
    }
    if (!strcmp(cmd, "alert-marker") && arg) {
        find_ring = true;
        return sip_ua_set_alert_marker(ua, arg) == SIP_OK;
    }
    if (!strcmp(cmd, "expect-alert")) {
        if (!last_alert) {
            printf("FAIL: the last incoming call did not carry the alert marker\n");
        }
        return last_alert;
    }
    if (!strcmp(cmd, "forward")) {
        snprintf(fwd_target, sizeof(fwd_target), "%s", arg ? arg : "");
        return true;
    }
    if (!strcmp(cmd, "forward-off")) {
        fwd_target[0] = '\0';
        return true;
    }
    if (!strcmp(cmd, "redirect") && arg) {
        return sip_call_redirect(ua, cur_call, arg) == SIP_OK;
    }
    if (!strcmp(cmd, "expect-rejected") && arg) {
        int want = atoi(arg);
        bool ok = last_rejected == want;
        uint64_t end = now_ms() + 10000;
        while (!ok && now_ms() < end) {
            run_for(20);
            ok = last_rejected == want;
        }
        if (!ok) {
            printf("FAIL rejected: last code %d, want %d\n", last_rejected, want);
        }
        last_rejected = 0;
        return ok;
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
    uint8_t codecs[SIP_UA_MAX_CODECS] = {0};
    int n_codecs = 0;
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
        } else if (i + 1 < argc && !strcmp(argv[i], "--codecs")) {
            char *c = argv[++i];
            while (*c && n_codecs < SIP_UA_MAX_CODECS) {
                codecs[n_codecs++] = (uint8_t)strtoul(c, &c, 10);
                if (*c == ',') {
                    c++;
                }
            }
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
        .cb = { .reg_state = reg_cb, .incoming_call = incoming_cb, .call_state = call_cb, .media = media_cb,
                .blf_state = blf_cb, .mwi = mwi_cb, .transfer = xfer_cb, .call_rejected = rejected_cb,
                .screen = screen_cb },
    };
    memcpy(cfg.codecs, codecs, sizeof(cfg.codecs));
    cfg.n_codecs = (uint8_t)n_codecs;
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
