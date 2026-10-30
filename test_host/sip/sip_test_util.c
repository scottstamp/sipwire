#include "sip_test_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sip_ua_priv.h"
#include "unity.h"

const char *FX_SDP_PCMU =
    "v=0\r\n"
    "o=srv 1 1 IN IP4 10.0.0.1\r\n"
    "s=-\r\n"
    "c=IN IP4 10.0.0.1\r\n"
    "t=0 0\r\n"
    "m=audio 20000 RTP/AVP 0 8 101\r\n"
    "a=rtpmap:0 PCMU/8000\r\n"
    "a=rtpmap:8 PCMA/8000\r\n"
    "a=rtpmap:101 telephone-event/8000\r\n"
    "a=fmtp:101 0-16\r\n"
    "a=ptime:20\r\n"
    "a=sendrecv\r\n";

const char *FX_SDP_G722 =
    "v=0\r\n"
    "o=srv 1 1 IN IP4 10.0.0.1\r\n"
    "s=-\r\n"
    "c=IN IP4 10.0.0.1\r\n"
    "t=0 0\r\n"
    "m=audio 20006 RTP/AVP 9 0 8 101\r\n"
    "a=rtpmap:9 G722/8000\r\n"
    "a=rtpmap:0 PCMU/8000\r\n"
    "a=rtpmap:8 PCMA/8000\r\n"
    "a=rtpmap:101 telephone-event/8000\r\n"
    "a=fmtp:101 0-16\r\n"
    "a=ptime:20\r\n"
    "a=sendrecv\r\n";

const char *FX_SDP_PCMA_ONLY =
    "v=0\r\n"
    "o=srv 1 1 IN IP4 10.0.0.1\r\n"
    "s=-\r\n"
    "c=IN IP4 10.0.0.1\r\n"
    "t=0 0\r\n"
    "m=audio 20002 RTP/AVP 8\r\n"
    "a=rtpmap:8 PCMA/8000\r\n";

const char *FX_SDP_G729_ONLY =
    "v=0\r\n"
    "o=srv 1 1 IN IP4 10.0.0.1\r\n"
    "s=-\r\n"
    "c=IN IP4 10.0.0.1\r\n"
    "t=0 0\r\n"
    "m=audio 20004 RTP/AVP 18\r\n"
    "a=rtpmap:18 G729/8000\r\n";

const char *FX_SDP_SENDONLY =
    "v=0\r\n"
    "o=srv 1 2 IN IP4 10.0.0.1\r\n"
    "s=-\r\n"
    "c=IN IP4 10.0.0.1\r\n"
    "t=0 0\r\n"
    "m=audio 20000 RTP/AVP 0 101\r\n"
    "a=rtpmap:0 PCMU/8000\r\n"
    "a=rtpmap:101 telephone-event/8000\r\n"
    "a=sendonly\r\n";

uint32_t fx_rand(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;
    return x;
}

static uint32_t rand_cb(void *ctx)
{
    return fx_rand(&((fx_t *)ctx)->rng);
}

static int send_cb(void *ctx, const sip_addr_t *to, const void *data, size_t len)
{
    fx_t *f = ctx;
    TEST_ASSERT_TRUE_MESSAGE(len <= SIP_MAX_MSG_OUT, "message too large");
    TEST_ASSERT_TRUE_MESSAGE(f->n_sent < FX_MAX_SENT, "too many sent messages");
    fx_sent_t *s = &f->sent[f->n_sent++];
    memcpy(s->buf, data, len);
    s->buf[len] = '\0';
    s->len = len;
    s->to = *to;
    /* never leak the password */
    TEST_ASSERT_NULL_MESSAGE(strstr(s->buf, "secret"), "password on the wire");
    return 0;
}

static void log_cb(void *ctx, sip_log_level_t lvl, const char *msg)
{
    (void)ctx;
    TEST_ASSERT_NULL_MESSAGE(strstr(msg, "secret"), "password in log");
    TEST_ASSERT_NULL_MESSAGE(strstr(msg, "response=\""), "digest response in log");
    if (getenv("SIP_TEST_LOG")) {
        printf("[%d] %s\n", (int)lvl, msg);
    }
}

static fx_ev_t *push_ev(fx_t *f, fx_ev_kind_t k, sip_call_handle_t call)
{
    TEST_ASSERT_TRUE(f->n_ev < FX_MAX_EVENTS);
    fx_ev_t *e = &f->ev[f->n_ev++];
    memset(e, 0, sizeof(*e));
    e->kind = k;
    e->call = call;
    return e;
}

static void reg_cb(void *ctx, sip_ua_reg_state_t st, int code)
{
    fx_ev_t *e = push_ev(ctx, FXE_REG, 0);
    e->state = st;
    e->code = code;
}

static void incoming_cb(void *ctx, sip_call_handle_t call, const char *number, const char *name)
{
    fx_t *f = ctx;
    fx_ev_t *e = push_ev(f, FXE_INCOMING, call);
    snprintf(e->number, sizeof(e->number), "%s", number);
    snprintf(e->display, sizeof(e->display), "%s", name);
    if (f->auto_answer) {
        TEST_ASSERT_EQUAL_INT(SIP_OK, sip_call_answer(f->ua, call));
    }
}

static void call_cb(void *ctx, sip_call_handle_t call, const sip_call_info_t *info)
{
    fx_t *f = ctx;
    fx_ev_t *e = push_ev(f, FXE_CALL, call);
    e->state = info->state;
    e->reason = info->end_reason;
    e->code = info->status_code;
    e->answer_after = info->answer_after;
    e->alert_match = info->alert_match;
    snprintf(e->number, sizeof(e->number), "%s", info->number);
    snprintf(e->display, sizeof(e->display), "%s", info->display_name);
    if (f->hangup_in_callback && info->state == SIP_UA_CALL_ACTIVE) {
        TEST_ASSERT_EQUAL_INT(SIP_OK, sip_call_hangup(f->ua, call));
    }
    if (f->destroy_in_callback && info->state == SIP_UA_CALL_ENDED) {
        sip_ua_destroy(f->ua);
        f->ua = NULL;
    }
}

static void media_cb(void *ctx, sip_call_handle_t call, const sip_media_info_t *mi)
{
    fx_ev_t *e = push_ev(ctx, FXE_MEDIA, call);
    e->media = *mi;
}

static void blf_cb(void *ctx, sip_sub_handle_t sub, const char *ext, sip_blf_state_t st)
{
    fx_ev_t *e = push_ev(ctx, FXE_BLF, sub);
    e->state = st;
    snprintf(e->number, sizeof(e->number), "%s", ext);
}

static void mwi_cb(void *ctx, bool waiting, int new_msgs, int old_msgs)
{
    fx_ev_t *e = push_ev(ctx, FXE_MWI, 0);
    e->state = waiting;
    e->reason = new_msgs;
    e->code = old_msgs;
}

static void xfer_cb(void *ctx, sip_call_handle_t call, sip_xfer_state_t st, int code)
{
    fx_ev_t *e = push_ev(ctx, FXE_XFER, call);
    e->state = st;
    e->code = code;
}

static void rejected_cb(void *ctx, const char *number, const char *display, int code)
{
    fx_ev_t *e = push_ev(ctx, FXE_REJECTED, 0);
    e->code = code;
    snprintf(e->number, sizeof(e->number), "%s", number);
    snprintf(e->display, sizeof(e->display), "%s", display);
}

static void screen_cb(void *ctx, const sip_screen_info_t *in, sip_screen_t *out)
{
    fx_t *f = ctx;
    TEST_ASSERT_EQUAL(SIP_SCREEN_DEFAULT, out->action);
    f->n_screen++;
    f->screen_in = *in;
    snprintf(f->screen_number, sizeof(f->screen_number), "%s", in->number);
    f->screen_in.number = f->screen_number;
    f->screen_in.display_name = "";
    if (f->screen_on) {
        *out = f->screen;
    }
}

void fx_init(fx_t *f, void (*cfg_tweak)(sip_ua_config_t *cfg))
{
    memset(f, 0, sizeof(*f));
    f->rng = 0x12345678;
    f->now = 1000;
    f->server = (sip_addr_t){ FX_SERVER_IP, 5060 };
    sip_ua_config_t cfg = {
        .server_host = "10.0.0.1",
        .server_port = 5060,
        .user = "1001",
        .password = "secret",
        .display_name = "Alice",
        .expires = 300,
        .local_addr = { FX_LOCAL_IP, 5060 },
        .rtp_port = 4000,
        .send = send_cb,
        .send_ctx = f,
        .rand = rand_cb,
        .rand_ctx = f,
        .log = log_cb,
        .log_level = SIP_LOG_VERBOSE,
        .cb = {
            .reg_state = reg_cb,
            .incoming_call = incoming_cb,
            .call_state = call_cb,
            .media = media_cb,
            .blf_state = blf_cb,
            .mwi = mwi_cb,
            .transfer = xfer_cb,
            .call_rejected = rejected_cb,
            .screen = screen_cb,
            .ctx = f,
        },
    };
    if (cfg_tweak) {
        cfg_tweak(&cfg);
    }
    TEST_ASSERT_EQUAL_INT(SIP_OK, sip_ua_create(&cfg, &f->ua));
    sip_ua_tick(f->ua, f->now);
}

void fx_free(fx_t *f)
{
    if (f->ua) {
        sip_ua_destroy(f->ua);
        f->ua = NULL;
    }
}

/* Every tick in every test also checks sip_ua_next_deadline_ms():
 *  - a tick before the deadline must change nothing but the clock (else the
 *    deadline was too late: an event-driven caller would miss that timer);
 *  - a tick at or after the deadline must do something or move the deadline
 *    past now (else an event-driven caller would spin). */
static void fx_tick_checked(fx_t *f)
{
    static sip_ua_t before;
    uint64_t d = sip_ua_next_deadline_ms(f->ua);
    memcpy(&before, f->ua, sizeof(before));
    int sent = f->n_sent, ev = f->n_ev;
    sip_ua_tick(f->ua, f->now);
    if (f->ua == NULL) {
        return;
    }
    uint64_t now = f->ua->now;
    before.now = now;
    bool changed = f->n_sent != sent || f->n_ev != ev || memcmp(&before, f->ua, sizeof(before)) != 0;
    if (now < d && changed) {
        char msg[96];
        snprintf(msg, sizeof(msg), "tick at %llu changed state before the deadline %llu",
                 (unsigned long long)now, (unsigned long long)d);
        TEST_FAIL_MESSAGE(msg);
    }
    if (now >= d && !changed && sip_ua_next_deadline_ms(f->ua) <= now) {
        char msg[96];
        snprintf(msg, sizeof(msg), "deadline %llu due at %llu but the tick did nothing (spin)",
                 (unsigned long long)d, (unsigned long long)now);
        TEST_FAIL_MESSAGE(msg);
    }
}

void fx_advance(fx_t *f, uint64_t ms)
{
    uint64_t end = f->now + ms;
    while (f->now < end && f->ua) {
        f->now += 10;
        fx_tick_checked(f);
        for (; f->auto_reg && f->reg_scan < f->n_sent; f->reg_scan++) {
            if (strncmp(f->sent[f->reg_scan].buf, "REGISTER ", 9) == 0) {
                fx_reply(f, f->reg_scan, 200, "s1", "Expires: 300\r\n", NULL);
            }
        }
    }
}

int fx_find(const fx_t *f, const char *prefix, int nth)
{
    size_t pl = strlen(prefix);
    for (int i = f->n_sent - 1; i >= 0; i--) {
        if (strncmp(f->sent[i].buf, prefix, pl) == 0 && nth-- == 0) {
            return i;
        }
    }
    return -1;
}

int fx_count(const fx_t *f, const char *prefix)
{
    int n = 0;
    size_t pl = strlen(prefix);
    for (int i = 0; i < f->n_sent; i++) {
        n += strncmp(f->sent[i].buf, prefix, pl) == 0;
    }
    return n;
}

void fx_clear_sent(fx_t *f)
{
    f->n_sent = 0;
    f->reg_scan = 0;
}

static char parse_store[SIP_MAX_MSG_IN + 1];
static sip_msg_t parse_msg;

const sip_msg_t *fx_parse(const fx_t *f, int idx)
{
    TEST_ASSERT_TRUE_MESSAGE(idx >= 0 && idx < f->n_sent, "no such sent message");
    memcpy(parse_store, f->sent[idx].buf, f->sent[idx].len + 1);
    TEST_ASSERT_EQUAL_INT_MESSAGE(SIP_PARSE_OK, sip_msg_parse(&parse_msg, parse_store, f->sent[idx].len),
                                  f->sent[idx].buf);
    return &parse_msg;
}

const char *fx_hdr(const fx_t *f, int idx, const char *name)
{
    static char out[512];
    TEST_ASSERT_TRUE(idx >= 0 && idx < f->n_sent);
    const char *p = f->sent[idx].buf;
    size_t nl = strlen(name);
    while ((p = strstr(p, "\r\n")) != NULL) {
        p += 2;
        if (strncmp(p, name, nl) == 0 && p[nl] == ':') {
            const char *v = p + nl + 1;
            while (*v == ' ') {
                v++;
            }
            const char *e = strstr(v, "\r\n");
            size_t n = e ? (size_t)(e - v) : strlen(v);
            if (n >= sizeof(out)) {
                n = sizeof(out) - 1;
            }
            memcpy(out, v, n);
            out[n] = '\0';
            return out;
        }
    }
    return NULL;
}

void fx_inject(fx_t *f, const char *msg)
{
    if (f->ua) {
        sip_ua_on_packet(f->ua, msg, strlen(msg), &f->server);
    }
}

void fx_reply(fx_t *f, int idx, int code, const char *to_tag, const char *extra, const char *body)
{
    const sip_msg_t *m = fx_parse(f, idx);
    TEST_ASSERT_TRUE(m->is_request);
    static char out[4096];
    int n = snprintf(out, sizeof(out), "SIP/2.0 %d %s\r\n", code, sip_reason_phrase(code));
    for (int i = 0; i < m->n_via; i++) {
        n += snprintf(out + n, sizeof(out) - (size_t)n, "Via: %.*s\r\n", (int)m->via[i].raw.len, m->via[i].raw.p);
    }
    n += snprintf(out + n, sizeof(out) - (size_t)n, "From: %.*s\r\nTo: %.*s", (int)m->from.raw.len,
                  m->from.raw.p, (int)m->to.raw.len, m->to.raw.p);
    if (to_tag && m->to.tag.len == 0) {
        n += snprintf(out + n, sizeof(out) - (size_t)n, ";tag=%s", to_tag);
    }
    n += snprintf(out + n, sizeof(out) - (size_t)n, "\r\nCall-ID: %.*s\r\nCSeq: %lu %.*s\r\n",
                  (int)m->call_id.len, m->call_id.p, (unsigned long)m->cseq,
                  (int)m->cseq_method_str.len, m->cseq_method_str.p);
    if (m->method == SIP_METHOD_INVITE && code > 100 && code < 300) {
        n += snprintf(out + n, sizeof(out) - (size_t)n, "Contact: <sip:srv@10.0.0.1:5060>\r\n");
    }
    if (extra) {
        n += snprintf(out + n, sizeof(out) - (size_t)n, "%s", extra);
    }
    if (body) {
        n += snprintf(out + n, sizeof(out) - (size_t)n,
                      "Content-Type: application/sdp\r\nContent-Length: %u\r\n\r\n%s",
                      (unsigned)strlen(body), body);
    } else {
        n += snprintf(out + n, sizeof(out) - (size_t)n, "Content-Length: 0\r\n\r\n");
    }
    TEST_ASSERT_TRUE(n > 0 && (size_t)n < sizeof(out));
    fx_inject(f, out);
}

int fx_count_ev(const fx_t *f, fx_ev_kind_t kind)
{
    int n = 0;
    for (int i = 0; i < f->n_ev; i++) {
        n += f->ev[i].kind == kind;
    }
    return n;
}

const fx_ev_t *fx_last_ev(const fx_t *f, fx_ev_kind_t kind)
{
    for (int i = f->n_ev - 1; i >= 0; i--) {
        if (f->ev[i].kind == kind) {
            return &f->ev[i];
        }
    }
    return NULL;
}
