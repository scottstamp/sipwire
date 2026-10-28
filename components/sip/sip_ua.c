/* SIP user agent core: registration, calls, request dispatch.
 * Layering: sip_msg (parse/write) → sip_txn (RFC 3261 §17) → this file (TU:
 * UAC/UAS cores, dialogs via sip_dialog, SDP via sip_sdp). */

#include "sip_ua.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sip_auth.h"
#include "sip_dialog.h"
#include "sip_msg.h"
#include "sip_sdp.h"
#include "sip_txn.h"
#include "sip_ua_internal.h"

#define MAX_CALLS        2
#define EVQ_LEN          16
#define STR_MAX          64
#define TAG_LEN          16
#define REG_BACKOFF_MIN  5000u
#define REG_BACKOFF_MAX  300000u
#define ALLOW_HDR        "Allow: INVITE, ACK, BYE, CANCEL, OPTIONS, INFO\r\n"
#define UA_HDR           "User-Agent: " SIP_UA_USER_AGENT "\r\n"

typedef enum {
    OWN_NONE = 0,
    OWN_REG,
    OWN_KEEPALIVE,
    OWN_INVITE,        /* our initial INVITE */
    OWN_REINVITE,      /* our re-INVITE (hold) */
    OWN_CANCEL,
    OWN_BYE,
    OWN_SERVER_INVITE, /* incoming INVITE / re-INVITE */
} owner_t;

typedef enum {
    CS_FREE = 0,
    CS_OUT_CALLING,    /* INVITE sent, nothing back yet but maybe 100 */
    CS_OUT_EARLY,      /* 18x received */
    CS_IN_RINGING,     /* incoming, 180 sent */
    CS_CONFIRMED,      /* 2xx sent or received */
    CS_ENDING,         /* ENDED reported; protocol cleanup still running */
} call_st_t;

/* What a UAS needs to answer a request after the datagram is gone. */
typedef struct {
    char hdrs[800];            /* Via..., From, Call-ID, CSeq lines */
    uint16_t hdrs_len;
    char to[SIP_NAMEADDR_MAX + 96];
    bool to_has_tag;
    char rr[512];              /* Record-Route lines */
    uint16_t rr_len;
} resp_tpl_t;

/* What our SDP said last time, to decide on an o= version bump. */
typedef struct {
    uint32_t ip;
    uint16_t port;
    uint8_t pts[SIP_UA_MAX_CODECS];
    uint8_t n_pts;
    int16_t dtmf;
    uint8_t dir;
    bool valid;
} sdp_sig_t;

typedef struct {
    call_st_t st;
    sip_call_handle_t handle;
    bool outgoing;
    bool end_reported;
    int last_code;
    char number[STR_MAX];
    char display[STR_MAX];

    sip_dialog_t dlg;
    bool dlg_valid;

    /* UAC: initial INVITE */
    char ruri[SIP_URI_MAX];
    char to_na[SIP_URI_MAX + 2];
    char call_id[48];
    char local_tag[TAG_LEN + 1];
    uint32_t invite_cseq;
    uint32_t inv_txn;
    uint8_t auth_tries;
    bool got_provisional;
    bool cancel_pending;
    bool cancel_sent;
    char ack[SIP_MAX_MSG_OUT];
    uint16_t ack_len;
    uint32_t ack_cseq;

    /* UAS */
    resp_tpl_t tpl;
    bool awaiting_ack;         /* 2xx sent, ACK not yet seen */
    uint32_t uas_ack_cseq;
    uint32_t uas_inv_txn;
    bool bye_pending;          /* hang up once the ACK arrives */
    bool offer_in_2xx;         /* we offered in the 2xx; answer comes in ACK */

    /* media */
    uint16_t rtp_port;
    uint32_t sdp_id;
    uint32_t sdp_ver;
    sdp_sig_t sdp_last;        /* parameters of the last SDP we sent */
    sip_sdp_t offer;           /* remote offer awaiting our answer */
    bool have_offer;
    sip_media_info_t media;
    bool media_valid;
    bool local_hold;

    /* our re-INVITE */
    uint32_t reinv_txn;
    bool reinv_hold;
    uint8_t reinv_auth_tries;

    uint64_t deadline;         /* CS_ENDING cleanup */
} call_t;

typedef struct {
    sip_ua_reg_state_t state;
    int code;
    bool reported;
    sip_ua_reg_state_t rep_state;
    int rep_code;
    bool active;               /* app wants to be registered */
    bool unregistering;
    char call_id[48];
    char tag[TAG_LEN + 1];
    uint32_t cseq;
    uint32_t txn;
    uint32_t sent_expires;
    uint8_t auth_tries;
    uint8_t interval_tries;
    uint32_t expires_req;
    uint32_t granted;
    uint64_t t_refresh;
    uint64_t t_retry;
    uint32_t backoff_ms;
    uint64_t t_keepalive;
} reg_t;

typedef enum { EV_REG, EV_INCOMING, EV_CALL, EV_MEDIA } ev_kind_t;

typedef struct {
    ev_kind_t kind;
    sip_call_handle_t call;
    int state;
    int reason;
    int code;
    bool outgoing;
    char number[STR_MAX];
    char display[STR_MAX];
    sip_media_info_t media;
} ev_t;

struct sip_ua {
    /* configuration (copied) */
    char server_host[STR_MAX];
    uint16_t server_port;
    sip_addr_t server;
    char domain[STR_MAX];
    char user[STR_MAX];
    char auth_user[STR_MAX];
    char password[STR_MAX];
    char display[STR_MAX];
    uint32_t expires;
    sip_addr_t local;
    sip_addr_t contact;        /* = local unless NAT discovered */
    uint16_t rtp_port;
    sip_ua_rtp_port_fn rtp_alloc;
    void *rtp_ctx;
    uint8_t codecs[SIP_UA_MAX_CODECS];
    uint8_t n_codecs;
    int dtmf_pt;
    uint8_t ptime;
    uint32_t keepalive_s;
    sip_keepalive_mode_t ka_mode;
    bool nat_fix;
    bool any_source;
    sip_ua_send_fn send;
    void *send_ctx;
    sip_ua_rand_fn rand;
    void *rand_ctx;
    sip_ua_log_fn log;
    void *log_ctx;
    sip_log_level_t log_level;
    sip_ua_callbacks_t cb;

    /* derived */
    char from_na[SIP_NAMEADDR_MAX];  /* "Name" <sip:user@domain> */
    char registrar[SIP_URI_MAX];     /* sip:domain[:port] */
    char hostpart[STR_MAX + 8];      /* domain[:port] for URIs */

    /* state */
    uint64_t now;
    sip_txn_layer_t txl;
    reg_t reg;
    sip_auth_t auth;                 /* cached credentials */
    call_t calls[MAX_CALLS];
    sip_call_handle_t next_handle;

    ev_t evq[EVQ_LEN];
    int ev_head;
    int ev_count;
    int depth;
    bool draining;
    bool destroy_pending;

    /* scratch (kept off the stack) */
    sip_msg_t msg;
    char rx[SIP_MAX_MSG_IN + 1];
    char tx[SIP_MAX_MSG_OUT + 1];
    char sdp[700];
    resp_tpl_t tpl_scratch;
};

/* ------------------------------------------------------------------ */
/* small helpers                                                        */

bool sip_ip_parse(const char *s, uint32_t *ip)
{
    uint32_t v = 0;
    int parts = 0;
    const char *p = s;
    if (!s) {
        return false;
    }
    while (parts < 4) {
        if (*p < '0' || *p > '9') {
            return false;
        }
        uint32_t oct = 0;
        int digits = 0;
        while (*p >= '0' && *p <= '9' && digits < 4) {
            oct = oct * 10 + (uint32_t)(*p++ - '0');
            digits++;
        }
        if (digits > 3 || oct > 255) {
            return false;
        }
        v = (v << 8) | oct;
        parts++;
        if (parts < 4) {
            if (*p != '.') {
                return false;
            }
            p++;
        }
    }
    if (*p != '\0') {
        return false;
    }
    *ip = v;
    return true;
}

void sip_ip_format(uint32_t ip, char out[16])
{
    snprintf(out, 16, "%u.%u.%u.%u", (unsigned)(ip >> 24) & 255, (unsigned)(ip >> 16) & 255,
             (unsigned)(ip >> 8) & 255, (unsigned)ip & 255);
}

static void ua_log(sip_ua_t *ua, sip_log_level_t lvl, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

static void ua_log(sip_ua_t *ua, sip_log_level_t lvl, const char *fmt, ...)
{
    if (!ua->log || lvl > ua->log_level) {
        return;
    }
    char buf[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    ua->log(ua->log_ctx, lvl, buf);
}

#define LOGE(...) ua_log(ua, SIP_LOG_ERROR, __VA_ARGS__)
#define LOGW(...) ua_log(ua, SIP_LOG_WARN, __VA_ARGS__)
#define LOGI(...) ua_log(ua, SIP_LOG_INFO, __VA_ARGS__)
#define LOGD(...) ua_log(ua, SIP_LOG_DEBUG, __VA_ARGS__)

/* Dump a message line by line at VERBOSE, never showing credentials. */
static void log_msg(sip_ua_t *ua, const char *dir, const char *buf, size_t len, const sip_addr_t *peer)
{
    if (!ua->log || ua->log_level < SIP_LOG_VERBOSE) {
        return;
    }
    char ip[16];
    sip_ip_format(peer->ip, ip);
    ua_log(ua, SIP_LOG_VERBOSE, "%s %s:%u (%u bytes)", dir, ip, (unsigned)peer->port, (unsigned)len);
    const char *p = buf;
    const char *e = buf + len;
    while (p < e) {
        const char *nl = memchr(p, '\n', (size_t)(e - p));
        const char *le = nl ? nl : e;
        size_t n = (size_t)(le - p);
        if (n && p[n - 1] == '\r') {
            n--;
        }
        sip_str_t line = { p, n };
        const char *colon = memchr(p, ':', n);
        sip_str_t name = colon ? sip_str_trim((sip_str_t){ p, (size_t)(colon - p) }) : SIP_STR_NULL;
        if (colon && (sip_str_ieq_c(name, "Authorization") || sip_str_ieq_c(name, "Proxy-Authorization"))) {
            ua_log(ua, SIP_LOG_VERBOSE, "  %.*s: <redacted>", (int)name.len, name.p);
        } else {
            char lb[180];
            size_t m = line.len < sizeof(lb) - 1 ? line.len : sizeof(lb) - 1;
            for (size_t i = 0; i < m; i++) {
                unsigned char c = (unsigned char)line.p[i];
                lb[i] = (c >= 0x20 && c < 0x7f) ? (char)c : '.';
            }
            lb[m] = '\0';
            ua_log(ua, SIP_LOG_VERBOSE, "  %s", lb);
        }
        p = nl ? nl + 1 : e;
    }
}

static void send_raw(sip_ua_t *ua, const sip_addr_t *to, const char *buf, size_t len)
{
    log_msg(ua, "send", buf, len, to);
    if (ua->send(ua->send_ctx, to, buf, len) != 0) {
        LOGW("send failed (%u bytes)", (unsigned)len);
    }
}

static void rand_hex(sip_ua_t *ua, char *out, int words)
{
    for (int i = 0; i < words; i++) {
        snprintf(out + i * 8, 9, "%08lx", (unsigned long)ua->rand(ua->rand_ctx));
    }
}

static void new_branch(sip_ua_t *ua, char out[32])
{
    memcpy(out, "z9hG4bK", 7);
    rand_hex(ua, out + 7, 2);
}

static void new_tag(sip_ua_t *ua, char out[TAG_LEN + 1])
{
    rand_hex(ua, out, 2);
}

static void new_call_id(sip_ua_t *ua, char out[48])
{
    char ip[16];
    rand_hex(ua, out, 3);
    sip_ip_format(ua->local.ip, ip);
    snprintf(out + 24, 48 - 24, "@%s", ip);
}

static void sent_by(sip_ua_t *ua, char out[24])
{
    char ip[16];
    sip_ip_format(ua->local.ip, ip);
    snprintf(out, 24, "%s:%u", ip, (unsigned)ua->local.port);
}

static void w_via(sip_ua_t *ua, sip_wbuf_t *w, const char *branch)
{
    char sb[24];
    sent_by(ua, sb);
    sip_w_fmt(w, "Via: SIP/2.0/UDP %s;rport;branch=%s\r\n", sb, branch);
}

static void w_contact(sip_ua_t *ua, sip_wbuf_t *w)
{
    char ip[16];
    sip_ip_format(ua->contact.ip, ip);
    sip_w_fmt(w, "Contact: <sip:%s@%s:%u>\r\n", ua->user, ip, (unsigned)ua->contact.port);
}

static void w_auth(sip_ua_t *ua, sip_wbuf_t *w, const char *method, const char *uri)
{
    if (!ua->auth.valid) {
        return;
    }
    char cnonce[17];
    rand_hex(ua, cnonce, 2);
    sip_auth_write_header(w, &ua->auth, ua->auth_user, ua->password, method, uri, cnonce);
}

static bool is_sdp(const sip_msg_t *m)
{
    if (!m->content_type.p || m->body.len == 0) {
        return false;
    }
    sip_str_t ct = m->content_type;
    const char *semi = memchr(ct.p, ';', ct.len);
    if (semi) {
        ct.len = (size_t)(semi - ct.p);
    }
    return sip_str_ieq_c(sip_str_trim(ct), "application/sdp");
}

/* ------------------------------------------------------------------ */
/* events + re-entrancy                                                 */

static ev_t *ev_push(sip_ua_t *ua, ev_kind_t kind, sip_call_handle_t call)
{
    if (ua->ev_count == EVQ_LEN) {
        LOGE("event queue overflow, dropping event");
        return NULL;
    }
    ev_t *e = &ua->evq[(ua->ev_head + ua->ev_count) % EVQ_LEN];
    ua->ev_count++;
    memset(e, 0, sizeof(*e));
    e->kind = kind;
    e->call = call;
    return e;
}

static void free_ua(sip_ua_t *ua)
{
    memset(ua->password, 0, sizeof(ua->password));
    memset(&ua->auth, 0, sizeof(ua->auth));
    free(ua);
}

static void enter(sip_ua_t *ua)
{
    ua->depth++;
}

/* Deliver queued callbacks once the outermost entry point is done. */
static void leave(sip_ua_t *ua)
{
    if (ua->depth == 1 && !ua->draining) {
        ua->draining = true;
        while (ua->ev_count > 0 && !ua->destroy_pending) {
            ev_t e = ua->evq[ua->ev_head];
            ua->ev_head = (ua->ev_head + 1) % EVQ_LEN;
            ua->ev_count--;
            const sip_ua_callbacks_t *cb = &ua->cb;
            switch (e.kind) {
            case EV_REG:
                if (cb->reg_state) {
                    cb->reg_state(cb->ctx, (sip_ua_reg_state_t)e.state, e.code);
                }
                break;
            case EV_INCOMING:
                if (cb->incoming_call) {
                    cb->incoming_call(cb->ctx, e.call, e.number, e.display);
                }
                break;
            case EV_CALL:
                if (cb->call_state) {
                    sip_call_info_t info = {
                        .state = (sip_ua_call_state_t)e.state,
                        .end_reason = (sip_ua_end_reason_t)e.reason,
                        .status_code = e.code,
                        .outgoing = e.outgoing,
                        .number = e.number,
                        .display_name = e.display,
                    };
                    cb->call_state(cb->ctx, e.call, &info);
                }
                break;
            case EV_MEDIA:
                if (cb->media) {
                    cb->media(cb->ctx, e.call, &e.media);
                }
                break;
            }
        }
        ua->draining = false;
    }
    ua->depth--;
    if (ua->depth == 0 && ua->destroy_pending) {
        free_ua(ua);
    }
}

/* ------------------------------------------------------------------ */
/* calls: bookkeeping                                                   */

static call_t *find_call(sip_ua_t *ua, sip_call_handle_t h)
{
    if (h <= 0) {
        return NULL;
    }
    for (int i = 0; i < MAX_CALLS; i++) {
        if (ua->calls[i].st != CS_FREE && ua->calls[i].handle == h) {
            return &ua->calls[i];
        }
    }
    return NULL;
}

static call_t *find_call_by_dialog(sip_ua_t *ua, const sip_msg_t *m)
{
    for (int i = 0; i < MAX_CALLS; i++) {
        call_t *c = &ua->calls[i];
        if (c->st != CS_FREE && c->dlg_valid && sip_dialog_matches_request(&c->dlg, m)) {
            return c;
        }
    }
    return NULL;
}

static bool have_visible_call(sip_ua_t *ua)
{
    for (int i = 0; i < MAX_CALLS; i++) {
        if (ua->calls[i].st != CS_FREE && ua->calls[i].st != CS_ENDING) {
            return true;
        }
    }
    return false;
}

static call_t *call_alloc(sip_ua_t *ua)
{
    for (int i = 0; i < MAX_CALLS; i++) {
        call_t *c = &ua->calls[i];
        if (c->st == CS_FREE) {
            memset(c, 0, sizeof(*c));
            c->handle = ua->next_handle++;
            if (ua->next_handle <= 0) {
                ua->next_handle = 1;
            }
            c->sdp_id = ua->rand(ua->rand_ctx) & 0x7fffffff;
            c->sdp_ver = 1;
            c->rtp_port = ua->rtp_alloc ? ua->rtp_alloc(ua->rtp_ctx) : ua->rtp_port;
            return c;
        }
    }
    return NULL;
}

static void call_free(call_t *c)
{
    c->st = CS_FREE;
    c->handle = 0;
}

static sip_ua_call_state_t public_state(const call_t *c)
{
    switch (c->st) {
    case CS_OUT_CALLING: return SIP_UA_CALL_DIALING;
    case CS_OUT_EARLY: return SIP_UA_CALL_RINGING_OUT;
    case CS_IN_RINGING: return SIP_UA_CALL_RINGING_IN;
    case CS_CONFIRMED: return c->local_hold ? SIP_UA_CALL_HELD : SIP_UA_CALL_ACTIVE;
    case CS_ENDING: return SIP_UA_CALL_ENDED;
    default: return SIP_UA_CALL_IDLE;
    }
}

static void emit_call(sip_ua_t *ua, call_t *c, sip_ua_call_state_t st, sip_ua_end_reason_t reason, int code)
{
    ev_t *e = ev_push(ua, EV_CALL, c->handle);
    if (!e) {
        return;
    }
    e->state = st;
    e->reason = reason;
    e->code = code;
    e->outgoing = c->outgoing;
    strcpy(e->number, c->number);
    strcpy(e->display, c->display);
}

static void emit_state(sip_ua_t *ua, call_t *c)
{
    emit_call(ua, c, public_state(c), SIP_END_NONE, c->last_code);
}

/* Report ENDED (once). release: free the slot now; otherwise keep it in
 * CS_ENDING until protocol cleanup is done or the deadline passes. */
static void end_call(sip_ua_t *ua, call_t *c, sip_ua_end_reason_t reason, int code, bool release)
{
    if (!c->end_reported) {
        c->end_reported = true;
        LOGI("call %d ended (reason %d, code %d)", c->handle, (int)reason, code);
        emit_call(ua, c, SIP_UA_CALL_ENDED, reason, code);
    }
    if (release) {
        call_free(c);
    } else {
        c->st = CS_ENDING;
        c->deadline = ua->now + 64u * SIP_T1_MS;
    }
}

static void emit_media(sip_ua_t *ua, call_t *c, const sip_sdp_neg_t *neg, sip_media_dir_t dir, bool early)
{
    sip_media_info_t mi;
    memset(&mi, 0, sizeof(mi));
    mi.remote_ip = neg->ip;
    sip_ip_format(neg->ip, mi.remote_ip_str);
    mi.remote_port = neg->port;
    mi.local_port = c->rtp_port;
    mi.payload_type = neg->pt;
    mi.dtmf_pt = (int16_t)neg->dtmf_pt;
    mi.ptime_ms = neg->ptime ? neg->ptime : ua->ptime;
    mi.direction = dir;
    mi.early = early;
    if (c->media_valid && memcmp(&mi, &c->media, sizeof(mi)) == 0) {
        return;
    }
    c->media = mi;
    c->media_valid = true;
    LOGI("call %d media: %s:%u pt %u dtmf %d %s%s", c->handle, mi.remote_ip_str,
         (unsigned)mi.remote_port, (unsigned)mi.payload_type, (int)mi.dtmf_pt,
         sip_sdp_dir_name(dir), early ? " (early)" : "");
    ev_t *e = ev_push(ua, EV_MEDIA, c->handle);
    if (e) {
        e->media = mi;
    }
}

/* Build our SDP into ua->sdp. answer_to: remote offer (NULL = we offer). */
static int make_sdp(sip_ua_t *ua, call_t *c, const sip_sdp_t *answer_to, const sip_sdp_neg_t *neg,
                    sip_media_dir_t dir)
{
    sip_sdp_local_t l = {
        .ip = ua->contact.ip,
        .port = c->rtp_port,
        .sess_id = c->sdp_id,
        .ptime = ua->ptime,
        .dir = dir,
        .offer = answer_to,
    };
    uint8_t one;
    if (answer_to) {
        one = neg->pt;
        l.pts = &one;
        l.n_pts = 1;
        l.dtmf_pt = neg->dtmf_pt;
    } else {
        l.pts = ua->codecs;
        l.n_pts = ua->n_codecs;
        l.dtmf_pt = ua->dtmf_pt;
    }
    /* o= version must change whenever the description changes (RFC 3264 §8) */
    sdp_sig_t cur;
    memset(&cur, 0, sizeof(cur));
    cur.ip = l.ip;
    cur.port = l.port;
    memcpy(cur.pts, l.pts, (size_t)l.n_pts);
    cur.n_pts = (uint8_t)l.n_pts;
    cur.dtmf = (int16_t)l.dtmf_pt;
    cur.dir = (uint8_t)dir;
    cur.valid = true;
    if (c->sdp_last.valid && memcmp(&cur, &c->sdp_last, sizeof(cur)) != 0) {
        c->sdp_ver++;
    }
    c->sdp_last = cur;
    l.sess_ver = c->sdp_ver;
    return sip_sdp_build(ua->sdp, sizeof(ua->sdp), &l);
}

static sip_media_dir_t want_dir(const call_t *c)
{
    return c->local_hold ? SIP_DIR_SENDONLY : SIP_DIR_SENDRECV;
}

/* ------------------------------------------------------------------ */
/* UAS responses                                                        */

enum {
    RF_CONTACT = 1,
    RF_RR = 2,
    RF_ALLOW = 4,
};

/* Top Via with RFC 3581 rport/received filled in. */
static void w_resp_via(sip_wbuf_t *w, const sip_via_t *v, const sip_addr_t *src, bool top)
{
    sip_w_cstr(w, "Via: ");
    if (top && v->rport == 0) {
        /* replace the empty ";rport" with ";rport=<port>", add received */
        const char *p = v->raw.p;
        const char *e = v->raw.p + v->raw.len;
        const char *hit = NULL;
        for (const char *q = v->params.p; q + 6 <= e; q++) {
            if (q[0] == ';' && sip_str_ieq_c((sip_str_t){ q + 1, 5 }, "rport") &&
                (q + 6 == e || !sip_is_token_char(q[6])) && (q + 6 == e || q[6] != '=')) {
                hit = q + 6;
                break;
            }
        }
        if (hit) {
            char ip[16];
            sip_ip_format(src->ip, ip);
            sip_w_mem(w, p, (size_t)(hit - p));
            sip_w_fmt(w, "=%u", (unsigned)src->port);
            sip_w_mem(w, hit, (size_t)(e - hit));
            if (!v->received.p) {
                sip_w_fmt(w, ";received=%s", ip);
            }
            sip_w_cstr(w, "\r\n");
            return;
        }
    }
    sip_w_str(w, v->raw);
    sip_w_cstr(w, "\r\n");
}

static int tpl_from_msg(resp_tpl_t *tpl, const sip_msg_t *m, const sip_addr_t *src)
{
    sip_wbuf_t w;
    sip_w_init(&w, tpl->hdrs, sizeof(tpl->hdrs));
    for (int i = 0; i < m->n_via; i++) {
        w_resp_via(&w, &m->via[i], src, i == 0);
    }
    sip_w_cstr(&w, "From: ");
    sip_w_str(&w, m->from.raw);
    sip_w_cstr(&w, "\r\nCall-ID: ");
    sip_w_str(&w, m->call_id);
    sip_w_fmt(&w, "\r\nCSeq: %lu ", (unsigned long)m->cseq);
    sip_w_str(&w, m->cseq_method_str);
    sip_w_cstr(&w, "\r\n");
    if (w.overflow) {
        return -1;
    }
    tpl->hdrs_len = (uint16_t)w.len;
    if (sip_str_copy(tpl->to, sizeof(tpl->to), m->to.raw) < 0) {
        return -1;
    }
    tpl->to_has_tag = m->to.tag.len > 0;
    sip_w_init(&w, tpl->rr, sizeof(tpl->rr));
    for (int i = 0; i < m->n_rr; i++) {
        sip_w_cstr(&w, "Record-Route: ");
        sip_w_str(&w, m->rr[i].raw);
        sip_w_cstr(&w, "\r\n");
    }
    if (w.overflow) {
        return -1;
    }
    tpl->rr_len = (uint16_t)w.len;
    return 0;
}

/* Build and send a response. t may be NULL (stateless). */
static void respond_tpl(sip_ua_t *ua, sip_txn_t *t, const resp_tpl_t *tpl, const sip_addr_t *peer,
                        int code, const char *to_tag, unsigned flags, const char *extra,
                        const char *body, size_t body_len)
{
    sip_wbuf_t w;
    sip_w_init(&w, ua->tx, sizeof(ua->tx));
    sip_w_fmt(&w, "SIP/2.0 %d %s\r\n", code, sip_reason_phrase(code));
    sip_w_mem(&w, tpl->hdrs, tpl->hdrs_len);
    sip_w_fmt(&w, "To: %s", tpl->to);
    if (!tpl->to_has_tag && to_tag && to_tag[0] && code != 100) {
        sip_w_fmt(&w, ";tag=%s", to_tag);
    }
    sip_w_cstr(&w, "\r\n");
    if (flags & RF_RR) {
        sip_w_mem(&w, tpl->rr, tpl->rr_len);
    }
    if (flags & RF_CONTACT) {
        w_contact(ua, &w);
    }
    if (flags & RF_ALLOW) {
        sip_w_cstr(&w, ALLOW_HDR "Accept: application/sdp\r\n");
    }
    sip_w_cstr(&w, UA_HDR);
    if (extra) {
        sip_w_cstr(&w, extra);
    }
    if (body && body_len) {
        sip_w_fmt(&w, "Content-Type: application/sdp\r\nContent-Length: %u\r\n\r\n", (unsigned)body_len);
        sip_w_mem(&w, body, body_len);
    } else {
        sip_w_cstr(&w, "Content-Length: 0\r\n\r\n");
    }
    if (w.overflow) {
        LOGE("response %d does not fit", code);
        return;
    }
    if (t) {
        log_msg(ua, "send", ua->tx, w.len, &t->peer);
        sip_txn_respond(&ua->txl, ua->now, t, code, ua->tx, w.len);
    } else {
        send_raw(ua, peer, ua->tx, w.len);
    }
}

/* Respond to the request currently in ua->msg. */
static void respond_msg(sip_ua_t *ua, sip_txn_t *t, const sip_msg_t *m, const sip_addr_t *from,
                        int code, unsigned flags, const char *extra)
{
    char tag[TAG_LEN + 1];
    if (tpl_from_msg(&ua->tpl_scratch, m, from) != 0) {
        LOGW("cannot build response template");
        return;
    }
    new_tag(ua, tag);
    respond_tpl(ua, t, &ua->tpl_scratch, from, code, tag, flags, extra, NULL, 0);
}

/* response on a call's INVITE server transaction (tag = dialog local tag) */
static void call_respond(sip_ua_t *ua, call_t *c, int code, unsigned flags, const char *body, size_t len)
{
    sip_txn_t *t = sip_txn_find(&ua->txl, c->uas_inv_txn);
    if (!t) {
        LOGW("call %d: INVITE transaction gone, cannot send %d", c->handle, code);
        return;
    }
    respond_tpl(ua, t, &c->tpl, &t->peer, code, c->dlg.local_tag, flags, NULL, body, len);
}

/* ------------------------------------------------------------------ */
/* authentication retry policy                                          */

/* Retry once per challenge; a stale=true challenge may be retried again
 * (bounded). Adopts the new challenge as cached credentials. */
static bool auth_retry(sip_ua_t *ua, uint8_t *tries, const sip_msg_t *m)
{
    sip_auth_t ch;
    if (sip_auth_from_response(m, &ch) != 0) {
        LOGW("no usable digest challenge in %d", m->status);
        return false;
    }
    if (*tries > 0 && !(ch.stale && *tries < 3)) {
        LOGW("authentication rejected (%d)", m->status);
        return false;
    }
    (*tries)++;
    ua->auth = ch;
    return true;
}

/* ------------------------------------------------------------------ */
/* registration                                                         */

static void reg_set_state(sip_ua_t *ua, sip_ua_reg_state_t st, int code)
{
    ua->reg.state = st;
    ua->reg.code = code;
    if (!ua->reg.reported || ua->reg.rep_state != st || ua->reg.rep_code != code) {
        ua->reg.reported = true;
        ua->reg.rep_state = st;
        ua->reg.rep_code = code;
        ev_t *e = ev_push(ua, EV_REG, 0);
        if (e) {
            e->state = st;
            e->code = code;
        }
    }
}

static uint32_t refresh_delay_s(uint32_t e)
{
    /* refresh at min(expires/2, expires-30); never earlier than 1 s */
    uint32_t half = e / 2;
    uint32_t d = (e > 30 && e - 30 < half) ? e - 30 : half;
    return d ? d : 1;
}

static void reg_fail(sip_ua_t *ua, int code)
{
    sip_ua_reg_state_t prev = ua->reg.state;
    reg_set_state(ua, SIP_UA_REG_FAILED, code);
    ua->reg.backoff_ms = ua->reg.backoff_ms ? ua->reg.backoff_ms * 2 : REG_BACKOFF_MIN;
    if (ua->reg.backoff_ms > REG_BACKOFF_MAX) {
        ua->reg.backoff_ms = REG_BACKOFF_MAX;
    }
    ua->reg.t_retry = ua->now + ua->reg.backoff_ms;
    ua->reg.t_refresh = 0;
    LOGW("registration failed (%d), retry in %u s%s", code, (unsigned)(ua->reg.backoff_ms / 1000),
         prev == SIP_UA_REG_REGISTERED ? " (was registered)" : "");
}

static int reg_send(sip_ua_t *ua, uint32_t expires)
{
    char branch[32];
    sip_wbuf_t w;
    new_branch(ua, branch);
    uint32_t cseq = ++ua->reg.cseq;
    sip_w_init(&w, ua->tx, sizeof(ua->tx));
    sip_w_fmt(&w, "REGISTER %s SIP/2.0\r\n", ua->registrar);
    w_via(ua, &w, branch);
    sip_w_cstr(&w, "Max-Forwards: 70\r\n");
    sip_w_fmt(&w, "From: %s;tag=%s\r\n", ua->from_na, ua->reg.tag);
    sip_w_fmt(&w, "To: %s\r\n", ua->from_na);
    sip_w_fmt(&w, "Call-ID: %s\r\n", ua->reg.call_id);
    sip_w_fmt(&w, "CSeq: %lu REGISTER\r\n", (unsigned long)cseq);
    w_contact(ua, &w);
    sip_w_fmt(&w, "Expires: %lu\r\n", (unsigned long)expires);
    sip_w_cstr(&w, ALLOW_HDR UA_HDR);
    w_auth(ua, &w, "REGISTER", ua->registrar);
    sip_w_cstr(&w, "Content-Length: 0\r\n\r\n");
    if (w.overflow) {
        LOGE("REGISTER does not fit");
        return -1;
    }
    sip_txn_t *t = sip_txn_client_start(&ua->txl, ua->now, SIP_METHOD_REGISTER, branch, cseq,
                                        &ua->server, ua->tx, w.len);
    if (!t) {
        LOGE("no free transaction for REGISTER");
        return -1;
    }
    log_msg(ua, "send", ua->tx, w.len, &ua->server);
    t->owner = OWN_REG;
    ua->reg.txn = t->id;
    ua->reg.sent_expires = expires;
    LOGD("REGISTER expires=%lu cseq=%lu", (unsigned long)expires, (unsigned long)cseq);
    return 0;
}

static void reg_start(sip_ua_t *ua)
{
    if (reg_send(ua, ua->reg.expires_req) != 0) {
        reg_fail(ua, 0);
    }
}

/* expires granted for our contact (§10.2.4) */
static uint32_t granted_expires(sip_ua_t *ua, const sip_msg_t *m)
{
    char ip[16];
    sip_ip_format(ua->contact.ip, ip);
    int any = -1;
    for (int i = 0; i < m->n_contact; i++) {
        const sip_nameaddr_t *c = &m->contact[i];
        if (c->star || c->expires < 0) {
            continue;
        }
        sip_uri_t u;
        if (sip_uri_parse(c->uri, &u) == 0 && sip_str_eq(u.user, sip_str_c(ua->user)) &&
            sip_str_eq(u.host, sip_str_c(ip)) && (u.port ? u.port : 5060) == ua->contact.port) {
            return (uint32_t)c->expires;
        }
        if (any < 0) {
            any = c->expires;
        }
    }
    if (any >= 0 && m->n_contact == 1) {
        return (uint32_t)any; /* rewritten by the registrar (rewrite_contact) */
    }
    if (m->expires >= 0) {
        return (uint32_t)m->expires;
    }
    if (any >= 0) {
        return (uint32_t)any;
    }
    return ua->reg.sent_expires;
}

static void reg_on_response(sip_ua_t *ua, sip_txn_t *t, const sip_msg_t *m)
{
    int code = m->status;
    if (t->id != ua->reg.txn || code < 200) {
        return;
    }
    ua->reg.txn = 0;
    if (code == 401 || code == 407) {
        if (auth_retry(ua, &ua->reg.auth_tries, m)) {
            if (reg_send(ua, ua->reg.sent_expires) != 0) {
                reg_fail(ua, code);
            }
            return;
        }
    }
    if (code == 423 && m->min_expires > 0 && !ua->reg.unregistering && ua->reg.interval_tries < 2) {
        ua->reg.interval_tries++;
        ua->reg.expires_req = (uint32_t)m->min_expires;
        if (reg_send(ua, ua->reg.expires_req) != 0) {
            reg_fail(ua, code);
        }
        return;
    }
    ua->reg.auth_tries = 0;
    ua->reg.interval_tries = 0;
    if (ua->reg.unregistering) {
        ua->reg.unregistering = false;
        LOGI("unregistered (%d)", code);
        reg_set_state(ua, SIP_UA_REG_NONE, code);
        return;
    }
    if (code >= 300) {
        reg_fail(ua, code);
        return;
    }
    uint32_t granted = granted_expires(ua, m);
    if (granted == 0) {
        granted = ua->reg.sent_expires;
    }
    ua->reg.granted = granted;
    ua->reg.backoff_ms = 0;
    ua->reg.t_retry = 0;
    ua->reg.t_refresh = ua->now + (uint64_t)refresh_delay_s(granted) * 1000u;
    if (ua->reg.state != SIP_UA_REG_REGISTERED) {
        LOGI("registered, expires %lu s", (unsigned long)granted);
        ua->reg.t_keepalive = ua->now + (uint64_t)ua->keepalive_s * 1000u;
    }
    reg_set_state(ua, SIP_UA_REG_REGISTERED, code);

    /* NAT: what the registrar saw (RFC 3581) vs. what we advertise */
    const sip_via_t *v = &m->via[0];
    if (ua->nat_fix && (v->received.len || v->rport > 0)) {
        sip_addr_t pub = ua->local;
        char tmp[16];
        uint32_t ip;
        if (v->received.len && sip_str_copy(tmp, sizeof(tmp), v->received) >= 0 && sip_ip_parse(tmp, &ip)) {
            pub.ip = ip;
        }
        if (v->rport > 0) {
            pub.port = (uint16_t)v->rport;
        }
        if (pub.ip != ua->contact.ip || pub.port != ua->contact.port) {
            sip_ip_format(pub.ip, tmp);
            LOGI("NAT detected: public address %s:%u, re-registering", tmp, (unsigned)pub.port);
            ua->contact = pub;
            ua->reg.t_refresh = 0;
            reg_start(ua);
        }
    }
}

static void keepalive_send(sip_ua_t *ua)
{
    if (ua->ka_mode == SIP_KEEPALIVE_CRLF) {
        send_raw(ua, &ua->server, "\r\n\r\n", 4);
        return;
    }
    char branch[32], tag[TAG_LEN + 1], cid[48];
    sip_wbuf_t w;
    new_branch(ua, branch);
    new_tag(ua, tag);
    new_call_id(ua, cid);
    sip_w_init(&w, ua->tx, sizeof(ua->tx));
    sip_w_fmt(&w, "OPTIONS %s SIP/2.0\r\n", ua->registrar);
    w_via(ua, &w, branch);
    sip_w_cstr(&w, "Max-Forwards: 70\r\n");
    sip_w_fmt(&w, "From: %s;tag=%s\r\n", ua->from_na, tag);
    sip_w_fmt(&w, "To: <%s>\r\n", ua->registrar);
    sip_w_fmt(&w, "Call-ID: %s\r\nCSeq: 1 OPTIONS\r\n", cid);
    w_contact(ua, &w);
    sip_w_cstr(&w, "Accept: application/sdp\r\n" UA_HDR "Content-Length: 0\r\n\r\n");
    if (w.overflow) {
        return;
    }
    sip_txn_t *t = sip_txn_client_start(&ua->txl, ua->now, SIP_METHOD_OPTIONS, branch, 1,
                                        &ua->server, ua->tx, w.len);
    if (t) {
        log_msg(ua, "send", ua->tx, w.len, &ua->server);
        t->owner = OWN_KEEPALIVE;
    }
}

static void reg_tick(sip_ua_t *ua)
{
    reg_t *r = &ua->reg;
    if (!r->active || r->txn) {
        return;
    }
    if (r->t_refresh && ua->now >= r->t_refresh) {
        r->t_refresh = 0;
        LOGD("refreshing registration");
        reg_start(ua);
    } else if (r->t_retry && ua->now >= r->t_retry) {
        r->t_retry = 0;
        reg_set_state(ua, SIP_UA_REG_REGISTERING, 0);
        reg_start(ua);
    }
    if (r->state == SIP_UA_REG_REGISTERED && ua->keepalive_s && ua->now >= r->t_keepalive) {
        r->t_keepalive = ua->now + (uint64_t)ua->keepalive_s * 1000u;
        keepalive_send(ua);
    }
}

/* ------------------------------------------------------------------ */
/* UAC: INVITE / ACK / CANCEL / BYE                                     */

static int invite_send(sip_ua_t *ua, call_t *c)
{
    char branch[32];
    sip_wbuf_t w;
    int sdp_len = make_sdp(ua, c, NULL, NULL, want_dir(c));
    if (sdp_len < 0) {
        return SIP_ERR_OVERFLOW;
    }
    new_branch(ua, branch);
    sip_w_init(&w, ua->tx, sizeof(ua->tx));
    sip_w_fmt(&w, "INVITE %s SIP/2.0\r\n", c->ruri);
    w_via(ua, &w, branch);
    sip_w_cstr(&w, "Max-Forwards: 70\r\n");
    sip_w_fmt(&w, "From: %s;tag=%s\r\n", ua->from_na, c->local_tag);
    sip_w_fmt(&w, "To: %s\r\n", c->to_na);
    sip_w_fmt(&w, "Call-ID: %s\r\n", c->call_id);
    sip_w_fmt(&w, "CSeq: %lu INVITE\r\n", (unsigned long)c->invite_cseq);
    w_contact(ua, &w);
    sip_w_cstr(&w, ALLOW_HDR UA_HDR);
    w_auth(ua, &w, "INVITE", c->ruri);
    sip_w_fmt(&w, "Content-Type: application/sdp\r\nContent-Length: %d\r\n\r\n", sdp_len);
    sip_w_mem(&w, ua->sdp, (size_t)sdp_len);
    if (w.overflow) {
        return SIP_ERR_OVERFLOW;
    }
    sip_txn_t *t = sip_txn_client_start(&ua->txl, ua->now, SIP_METHOD_INVITE, branch,
                                        c->invite_cseq, &ua->server, ua->tx, w.len);
    if (!t) {
        return SIP_ERR_NOMEM;
    }
    log_msg(ua, "send", ua->tx, w.len, &ua->server);
    t->owner = OWN_INVITE;
    t->owner_ref = c->handle;
    c->inv_txn = t->id;
    c->got_provisional = false;
    return SIP_OK;
}

/* ACK for a 2xx: end-to-end, new branch, built by the UA core (§13.2.2.4) */
static void send_ack_2xx(sip_ua_t *ua, call_t *c, uint32_t cseq, const char *body, size_t body_len)
{
    char branch[32], sb[24];
    sip_wbuf_t w;
    new_branch(ua, branch);
    sent_by(ua, sb);
    sip_w_init(&w, c->ack, sizeof(c->ack));
    sip_dialog_write_request(&c->dlg, &w, "ACK", cseq, sb, branch);
    sip_w_cstr(&w, UA_HDR);
    if (body && body_len) {
        sip_w_fmt(&w, "Content-Type: application/sdp\r\nContent-Length: %u\r\n\r\n", (unsigned)body_len);
        sip_w_mem(&w, body, body_len);
    } else {
        sip_w_cstr(&w, "Content-Length: 0\r\n\r\n");
    }
    if (w.overflow) {
        LOGE("ACK does not fit");
        c->ack_len = 0;
        return;
    }
    c->ack_len = (uint16_t)w.len;
    c->ack_cseq = cseq;
    send_raw(ua, &ua->server, c->ack, c->ack_len);
}

static void send_in_dialog(sip_ua_t *ua, call_t *c, const char *method, sip_method_t mid, owner_t owner,
                           const char *body, size_t body_len, bool contact)
{
    char branch[32], sb[24];
    sip_wbuf_t w;
    new_branch(ua, branch);
    sent_by(ua, sb);
    uint32_t cseq = ++c->dlg.local_cseq;
    sip_w_init(&w, ua->tx, sizeof(ua->tx));
    sip_dialog_write_request(&c->dlg, &w, method, cseq, sb, branch);
    if (contact) {
        w_contact(ua, &w);
        sip_w_cstr(&w, ALLOW_HDR);
    }
    sip_w_cstr(&w, UA_HDR);
    if (mid == SIP_METHOD_INVITE) {
        w_auth(ua, &w, method, c->dlg.remote_target);
    }
    if (body && body_len) {
        sip_w_fmt(&w, "Content-Type: application/sdp\r\nContent-Length: %u\r\n\r\n", (unsigned)body_len);
        sip_w_mem(&w, body, body_len);
    } else {
        sip_w_cstr(&w, "Content-Length: 0\r\n\r\n");
    }
    if (w.overflow) {
        LOGE("%s does not fit", method);
        return;
    }
    sip_txn_t *t = sip_txn_client_start(&ua->txl, ua->now, mid, branch, cseq, &ua->server, ua->tx, w.len);
    if (!t) {
        LOGE("no free transaction for %s", method);
        return;
    }
    log_msg(ua, "send", ua->tx, w.len, &ua->server);
    t->owner = owner;
    t->owner_ref = c->handle;
    if (owner == OWN_REINVITE) {
        c->reinv_txn = t->id;
    }
}

static void send_bye(sip_ua_t *ua, call_t *c)
{
    LOGI("call %d: sending BYE", c->handle);
    send_in_dialog(ua, c, "BYE", SIP_METHOD_BYE, OWN_BYE, NULL, 0, false);
}

static void send_cancel(sip_ua_t *ua, call_t *c)
{
    sip_txn_t *it = sip_txn_find(&ua->txl, c->inv_txn);
    if (!it || it->kind != SIP_TXN_CLIENT_INVITE) {
        return;
    }
    if (it->state != SIP_TS_PROCEEDING) {
        /* §9.1: no CANCEL before a provisional response */
        c->cancel_pending = it->state == SIP_TS_CALLING;
        return;
    }
    int n = sip_txn_build_cancel(it, ua->tx, sizeof(ua->tx));
    if (n < 0) {
        return;
    }
    sip_txn_t *t = sip_txn_client_start(&ua->txl, ua->now, SIP_METHOD_CANCEL, it->branch, it->cseq,
                                        &ua->server, ua->tx, (size_t)n);
    if (!t) {
        LOGE("no free transaction for CANCEL");
        return;
    }
    log_msg(ua, "send", ua->tx, (size_t)n, &ua->server);
    t->owner = OWN_CANCEL;
    t->owner_ref = c->handle;
    c->cancel_sent = true;
    c->cancel_pending = false;
    LOGI("call %d: CANCEL sent", c->handle);
}

/* Apply an SDP answer (to our offer). Returns false if unusable. */
static bool apply_answer(sip_ua_t *ua, call_t *c, const sip_msg_t *m, bool early)
{
    sip_sdp_t sdp;
    sip_sdp_neg_t neg;
    if (sip_sdp_parse(&sdp, m->body.p, m->body.len) != 0 ||
        sip_sdp_negotiate(&sdp, ua->codecs, ua->n_codecs, ua->dtmf_pt >= 0, false, &neg) != 0) {
        return false;
    }
    emit_media(ua, c, &neg, sip_sdp_dir_combine(want_dir(c), neg.remote_dir), early);
    return true;
}

static void on_invite_response(sip_ua_t *ua, call_t *c, sip_txn_t *t, const sip_msg_t *m)
{
    int code = m->status;
    if (t->id != c->inv_txn) {
        return; /* superseded (e.g. the challenged first INVITE) */
    }
    if (code < 200) {
        c->got_provisional = true;
        if (c->st == CS_ENDING) {
            if (c->cancel_pending) {
                send_cancel(ua, c);
            }
            return;
        }
        if (code == 100) {
            return;
        }
        if (m->to.tag.len && !c->dlg_valid &&
            sip_dialog_init_uac(&c->dlg, m, c->call_id, ua->from_na, c->local_tag, c->to_na, c->ruri,
                                c->invite_cseq) == 0) {
            c->dlg_valid = true; /* early dialog */
        }
        c->last_code = code;
        if (is_sdp(m)) {
            apply_answer(ua, c, m, true);
        }
        if (c->st == CS_OUT_CALLING) {
            c->st = CS_OUT_EARLY;
            emit_state(ua, c);
        }
        return;
    }
    if (code < 300) {
        if (c->dlg_valid && c->dlg.confirmed) {
            /* retransmitted 2xx: our ACK was lost (§13.2.2.4) */
            if (c->ack_len && m->cseq == c->ack_cseq) {
                send_raw(ua, &ua->server, c->ack, c->ack_len);
            }
            return;
        }
        if (sip_dialog_init_uac(&c->dlg, m, c->call_id, ua->from_na, c->local_tag, c->to_na, c->ruri,
                                c->invite_cseq) != 0) {
            LOGE("call %d: cannot create dialog from 2xx", c->handle);
            end_call(ua, c, SIP_END_ERROR, 500, true);
            return;
        }
        c->dlg_valid = true;
        c->dlg.confirmed = true;
        send_ack_2xx(ua, c, c->invite_cseq, NULL, 0);
        if (c->st == CS_ENDING) {
            /* hung up meanwhile (CANCEL lost the race with 200): ACK + BYE */
            LOGI("call %d: 2xx after hangup, sending BYE", c->handle);
            send_bye(ua, c);
            call_free(c);
            return;
        }
        bool ok = is_sdp(m) ? apply_answer(ua, c, m, false) : c->media_valid;
        if (!ok) {
            LOGW("call %d: no usable SDP answer", c->handle);
            send_bye(ua, c);
            end_call(ua, c, SIP_END_ERROR, 488, true);
            return;
        }
        if (c->media.early) {
            c->media.early = false;
            ev_t *e = ev_push(ua, EV_MEDIA, c->handle);
            if (e) {
                e->media = c->media;
            }
        }
        c->st = CS_CONFIRMED;
        c->last_code = code;
        LOGI("call %d established", c->handle);
        emit_state(ua, c);
        return;
    }
    /* 3xx-6xx: the transaction already sent the ACK */
    if ((code == 401 || code == 407) && c->st != CS_ENDING && auth_retry(ua, &c->auth_tries, m)) {
        c->invite_cseq++;
        c->dlg_valid = false;
        if (invite_send(ua, c) == SIP_OK) {
            return;
        }
    }
    if (c->st == CS_ENDING) {
        call_free(c); /* e.g. the 487 after our CANCEL */
        return;
    }
    end_call(ua, c, SIP_END_REJECTED, code, true);
}

static void on_reinvite_response(sip_ua_t *ua, call_t *c, sip_txn_t *t, const sip_msg_t *m)
{
    int code = m->status;
    if (code < 200) {
        return;
    }
    if (code < 300) {
        if (c->ack_len && c->ack_cseq == t->cseq) {
            send_raw(ua, &ua->server, c->ack, c->ack_len); /* 2xx retransmission */
            return;
        }
        send_ack_2xx(ua, c, t->cseq, NULL, 0);
        c->reinv_txn = 0;
        if (c->st != CS_CONFIRMED) {
            return;
        }
        sip_dialog_set_target(&c->dlg, m);
        bool changed = c->local_hold != c->reinv_hold;
        c->local_hold = c->reinv_hold;
        if (is_sdp(m) && !apply_answer(ua, c, m, false)) {
            LOGW("call %d: unusable answer to re-INVITE", c->handle);
        }
        if (changed) {
            emit_state(ua, c);
        }
        return;
    }
    c->reinv_txn = 0;
    if ((code == 401 || code == 407) && auth_retry(ua, &c->reinv_auth_tries, m)) {
        int n = make_sdp(ua, c, NULL, NULL, c->reinv_hold ? SIP_DIR_SENDONLY : SIP_DIR_SENDRECV);
        if (n > 0) {
            send_in_dialog(ua, c, "INVITE", SIP_METHOD_INVITE, OWN_REINVITE, ua->sdp, (size_t)n, true);
        }
        return;
    }
    LOGW("call %d: re-INVITE rejected (%d)", c->handle, code);
    if (code == 481 || code == 408) {
        /* §14.1: the dialog is gone */
        send_bye(ua, c);
        end_call(ua, c, SIP_END_REMOTE, code, true);
    }
}

/* ------------------------------------------------------------------ */
/* transaction user callbacks                                           */

static void tu_send(void *ctx, const sip_addr_t *to, const char *buf, size_t len)
{
    sip_ua_t *ua = ctx;
    if (ua->send(ua->send_ctx, to, buf, len) != 0) {
        LOGW("send failed (%u bytes)", (unsigned)len);
    }
}

static void tu_on_response(void *ctx, sip_txn_t *t, const sip_msg_t *m)
{
    sip_ua_t *ua = ctx;
    call_t *c = find_call(ua, t->owner_ref);
    switch ((owner_t)t->owner) {
    case OWN_REG:
        reg_on_response(ua, t, m);
        break;
    case OWN_KEEPALIVE:
        LOGD("keepalive OPTIONS answered %d", m->status);
        break;
    case OWN_INVITE:
        if (c) {
            on_invite_response(ua, c, t, m);
        }
        break;
    case OWN_REINVITE:
        if (c) {
            on_reinvite_response(ua, c, t, m);
        }
        break;
    case OWN_CANCEL:
        if (m->status >= 200) {
            LOGI("call %d: CANCEL answered %d", t->owner_ref, m->status);
        }
        break;
    case OWN_BYE:
        if (m->status >= 200) {
            LOGI("call %d: BYE answered %d", t->owner_ref, m->status);
        }
        break;
    default:
        break;
    }
}

static void tu_on_timeout(void *ctx, sip_txn_t *t)
{
    sip_ua_t *ua = ctx;
    call_t *c = find_call(ua, t->owner_ref);
    switch ((owner_t)t->owner) {
    case OWN_REG:
        if (t->id == ua->reg.txn) {
            ua->reg.txn = 0;
            if (ua->reg.unregistering) {
                ua->reg.unregistering = false;
                reg_set_state(ua, SIP_UA_REG_NONE, 0);
            } else {
                reg_fail(ua, 0);
            }
        }
        break;
    case OWN_KEEPALIVE:
        LOGW("keepalive OPTIONS timed out, re-registering");
        if (ua->reg.active && !ua->reg.txn) {
            reg_start(ua);
        }
        break;
    case OWN_INVITE:
        if (c && c->inv_txn == t->id) {
            if (c->st == CS_ENDING) {
                call_free(c);
            } else {
                end_call(ua, c, SIP_END_TIMEOUT, 408, true);
            }
        }
        break;
    case OWN_REINVITE:
        if (c && c->reinv_txn == t->id) {
            c->reinv_txn = 0;
            send_bye(ua, c);
            end_call(ua, c, SIP_END_TIMEOUT, 408, true);
        }
        break;
    case OWN_BYE:
        LOGW("call %d: BYE timed out", t->owner_ref);
        break;
    case OWN_SERVER_INVITE:
        if (c && t->state == SIP_TS_ACCEPTED && c->awaiting_ack && c->uas_inv_txn == t->id) {
            /* §13.3.1.4: no ACK for our 2xx → end the session */
            LOGW("call %d: no ACK for 2xx", c->handle);
            c->awaiting_ack = false;
            send_bye(ua, c);
            if (c->st == CS_ENDING) {
                call_free(c);
            } else {
                end_call(ua, c, SIP_END_TIMEOUT, 408, true);
            }
        }
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* UAS                                                                  */

static void copy_display(char *dst, size_t cap, sip_str_t d)
{
    size_t o = 0;
    for (size_t i = 0; i < d.len && o + 1 < cap; i++) {
        char ch = d.p[i];
        if (ch == '\\' && i + 1 < d.len) {
            ch = d.p[++i];
        }
        if ((unsigned char)ch < 0x20) {
            continue;
        }
        dst[o++] = ch;
    }
    dst[o] = '\0';
}

static void caller_identity(call_t *c, const sip_nameaddr_t *from)
{
    sip_uri_t u;
    c->number[0] = '\0';
    if (sip_uri_parse(from->uri, &u) == 0) {
        sip_str_t who = u.user.len ? u.user : u.host;
        if (sip_unescape(c->number, sizeof(c->number), who) < 0) {
            sip_str_copy(c->number, sizeof(c->number), (sip_str_t){ who.p, who.len < STR_MAX ? who.len : STR_MAX - 1 });
        }
    }
    copy_display(c->display, sizeof(c->display), from->display);
}

static void on_invite_new(sip_ua_t *ua, sip_txn_t *t, const sip_msg_t *m, const sip_addr_t *from)
{
    respond_msg(ua, t, m, from, 100, 0, NULL);
    if (m->body.len && !is_sdp(m)) {
        respond_msg(ua, t, m, from, 415, 0, "Accept: application/sdp\r\n");
        return;
    }
    call_t *c = have_visible_call(ua) ? NULL : call_alloc(ua);
    if (!c) {
        LOGI("busy: rejecting incoming INVITE with 486");
        respond_msg(ua, t, m, from, 486, 0, NULL);
        return;
    }
    char tag[TAG_LEN + 1];
    new_tag(ua, tag);
    if (sip_dialog_init_uas(&c->dlg, m, tag) != 0 || tpl_from_msg(&c->tpl, m, from) != 0) {
        respond_msg(ua, t, m, from, 400, 0, NULL);
        call_free(c);
        return;
    }
    c->dlg_valid = true;
    c->st = CS_IN_RINGING;
    c->outgoing = false;
    c->uas_inv_txn = t->id;
    t->owner = OWN_SERVER_INVITE;
    t->owner_ref = c->handle;
    caller_identity(c, &m->from);
    if (is_sdp(m)) {
        sip_sdp_neg_t neg;
        if (sip_sdp_parse(&c->offer, m->body.p, m->body.len) != 0 ||
            sip_sdp_negotiate(&c->offer, ua->codecs, ua->n_codecs, ua->dtmf_pt >= 0, true, &neg) != 0) {
            LOGI("incoming call: no acceptable media, 488");
            call_respond(ua, c, 488, 0, NULL, 0);
            call_free(c);
            return;
        }
        c->have_offer = true;
    }
    call_respond(ua, c, 180, RF_CONTACT | RF_RR, NULL, 0);
    c->last_code = 180;
    LOGI("incoming call %d from %s \"%s\"", c->handle, c->number, c->display);
    ev_t *e = ev_push(ua, EV_INCOMING, c->handle);
    if (e) {
        strcpy(e->number, c->number);
        strcpy(e->display, c->display);
    }
    emit_state(ua, c);
}

/* 200 OK to an INVITE/re-INVITE with our answer (or offer). */
static int answer_invite(sip_ua_t *ua, call_t *c, unsigned flags)
{
    sip_sdp_neg_t neg;
    int n;
    bool have_neg = false;
    if (c->have_offer) {
        if (sip_sdp_negotiate(&c->offer, ua->codecs, ua->n_codecs, ua->dtmf_pt >= 0, true, &neg) != 0) {
            return -488;
        }
        n = make_sdp(ua, c, &c->offer, &neg, sip_sdp_dir_combine(want_dir(c), neg.remote_dir));
        have_neg = true;
    } else {
        n = make_sdp(ua, c, NULL, NULL, want_dir(c)); /* delayed offer: answer comes in ACK */
        c->offer_in_2xx = true;
    }
    if (n < 0) {
        return -500;
    }
    c->awaiting_ack = true;
    c->uas_ack_cseq = c->dlg.remote_cseq;
    call_respond(ua, c, 200, flags, ua->sdp, (size_t)n);
    c->have_offer = false;
    if (have_neg) {
        emit_media(ua, c, &neg, sip_sdp_dir_combine(want_dir(c), neg.remote_dir), false);
    }
    return 0;
}

static void on_reinvite(sip_ua_t *ua, sip_txn_t *t, const sip_msg_t *m, const sip_addr_t *from, call_t *c)
{
    if (c->st != CS_CONFIRMED) {
        respond_msg(ua, t, m, from, c->st == CS_ENDING ? 481 : 491, 0, NULL);
        return;
    }
    if (c->reinv_txn || c->awaiting_ack) {
        respond_msg(ua, t, m, from, 491, 0, NULL); /* §14.2 glare */
        return;
    }
    if (m->body.len && !is_sdp(m)) {
        respond_msg(ua, t, m, from, 415, 0, "Accept: application/sdp\r\n");
        return;
    }
    if (tpl_from_msg(&c->tpl, m, from) != 0) {
        respond_msg(ua, t, m, from, 500, 0, NULL);
        return;
    }
    t->owner = OWN_SERVER_INVITE;
    t->owner_ref = c->handle;
    c->uas_inv_txn = t->id;
    sip_dialog_set_target(&c->dlg, m);
    c->have_offer = false;
    if (is_sdp(m)) {
        if (sip_sdp_parse(&c->offer, m->body.p, m->body.len) != 0) {
            call_respond(ua, c, 488, 0, NULL, 0);
            return;
        }
        c->have_offer = true;
    }
    int r = answer_invite(ua, c, RF_CONTACT | RF_ALLOW);
    if (r < 0) {
        c->have_offer = false;
        call_respond(ua, c, -r, 0, NULL, 0); /* session unchanged (§14.2) */
    } else {
        LOGI("call %d: re-INVITE answered", c->handle);
    }
}

static void on_ack(sip_ua_t *ua, const sip_msg_t *m)
{
    call_t *c = find_call_by_dialog(ua, m);
    if (!c || !c->awaiting_ack || m->cseq != c->uas_ack_cseq) {
        return;
    }
    c->awaiting_ack = false;
    sip_txn_t *t = sip_txn_find(&ua->txl, c->uas_inv_txn);
    if (t) {
        sip_txn_ack_2xx(t);
    }
    if (c->offer_in_2xx) {
        c->offer_in_2xx = false;
        sip_sdp_t sdp;
        sip_sdp_neg_t neg;
        if (!is_sdp(m) || sip_sdp_parse(&sdp, m->body.p, m->body.len) != 0 ||
            sip_sdp_negotiate(&sdp, ua->codecs, ua->n_codecs, ua->dtmf_pt >= 0, false, &neg) != 0) {
            LOGW("call %d: no usable answer in ACK", c->handle);
            send_bye(ua, c);
            end_call(ua, c, SIP_END_ERROR, 488, true);
            return;
        }
        if (c->st == CS_CONFIRMED) {
            emit_media(ua, c, &neg, sip_sdp_dir_combine(want_dir(c), neg.remote_dir), false);
        }
    }
    if (c->bye_pending) {
        send_bye(ua, c);
        call_free(c);
    }
}

static void on_cancel(sip_ua_t *ua, sip_txn_t *t, const sip_msg_t *m, const sip_addr_t *from)
{
    /* §9.2: the CANCEL matches the INVITE server transaction by branch */
    sip_txn_t *inv = NULL;
    for (int i = 0; i < SIP_MAX_TXN; i++) {
        sip_txn_t *x = &ua->txl.pool[i];
        if (x->id && x->kind == SIP_TXN_SERVER_INVITE && strcmp(x->branch, t->branch) == 0 &&
            strcmp(x->sent_by, t->sent_by) == 0) {
            inv = x;
            break;
        }
    }
    if (!inv) {
        respond_msg(ua, t, m, from, 481, 0, NULL);
        return;
    }
    call_t *c = find_call(ua, inv->owner_ref);
    if (tpl_from_msg(&ua->tpl_scratch, m, from) == 0) {
        respond_tpl(ua, t, &ua->tpl_scratch, from, 200, c ? c->dlg.local_tag : NULL, 0, NULL, NULL, 0);
    }
    if (c && c->st == CS_IN_RINGING && inv->state == SIP_TS_PROCEEDING) {
        call_respond(ua, c, 487, 0, NULL, 0);
        end_call(ua, c, SIP_END_CANCELLED, 487, true);
    }
}

static void on_in_dialog(sip_ua_t *ua, sip_txn_t *t, const sip_msg_t *m, const sip_addr_t *from, call_t *c)
{
    /* §12.2.2: CSeq must increase */
    if (c->dlg.remote_cseq_valid && m->cseq <= c->dlg.remote_cseq) {
        respond_msg(ua, t, m, from, 500, 0, NULL);
        return;
    }
    c->dlg.remote_cseq = m->cseq;
    c->dlg.remote_cseq_valid = true;
    switch (m->method) {
    case SIP_METHOD_BYE:
        respond_msg(ua, t, m, from, 200, 0, NULL);
        if (c->st == CS_IN_RINGING) {
            call_respond(ua, c, 487, 0, NULL, 0);
        }
        if (c->st == CS_ENDING) {
            call_free(c);
        } else {
            LOGI("call %d: remote hung up", c->handle);
            end_call(ua, c, SIP_END_REMOTE, 0, true);
        }
        break;
    case SIP_METHOD_INVITE:
        on_reinvite(ua, t, m, from, c);
        break;
    case SIP_METHOD_OPTIONS:
        respond_msg(ua, t, m, from, 200, RF_ALLOW, NULL);
        break;
    case SIP_METHOD_INFO:
        respond_msg(ua, t, m, from, 200, 0, NULL);
        break;
    default:
        respond_msg(ua, t, m, from, 501, RF_ALLOW, NULL);
        break;
    }
}

static void on_request(sip_ua_t *ua, const sip_msg_t *m, const sip_addr_t *from)
{
    sip_txn_t *t = sip_txn_server_match(&ua->txl, m);
    if (t && sip_txn_server_absorb(&ua->txl, ua->now, t, m)) {
        return; /* retransmission or ACK for a non-2xx */
    }
    if (m->method == SIP_METHOD_ACK) {
        on_ack(ua, m);
        return;
    }
    sip_uri_t ru;
    if (sip_uri_parse(m->ruri, &ru) != 0) {
        respond_msg(ua, NULL, m, from, 416, 0, NULL);
        return;
    }
    if (ru.headers.p) {
        respond_msg(ua, NULL, m, from, 400, 0, NULL); /* RFC 4475 escruri */
        return;
    }
    t = sip_txn_server_create(&ua->txl, ua->now, m, from);
    if (!t) {
        respond_msg(ua, NULL, m, from, 503, 0, "Retry-After: 5\r\n");
        return;
    }
    if (m->require.p && m->method != SIP_METHOD_CANCEL) {
        char extra[160];
        snprintf(extra, sizeof(extra), "Unsupported: %.*s\r\n",
                 (int)(m->require.len < 120 ? m->require.len : 120), m->require.p);
        respond_msg(ua, t, m, from, 420, 0, extra);
        return;
    }
    if (m->to.tag.len) {
        call_t *c = find_call_by_dialog(ua, m);
        if (!c) {
            respond_msg(ua, t, m, from, 481, 0, NULL);
            return;
        }
        on_in_dialog(ua, t, m, from, c);
        return;
    }
    switch (m->method) {
    case SIP_METHOD_INVITE:
        on_invite_new(ua, t, m, from);
        break;
    case SIP_METHOD_CANCEL:
        on_cancel(ua, t, m, from);
        break;
    case SIP_METHOD_OPTIONS:
        respond_msg(ua, t, m, from, 200, RF_ALLOW, NULL);
        break;
    case SIP_METHOD_BYE:
        respond_msg(ua, t, m, from, 481, 0, NULL);
        break;
    case SIP_METHOD_NOTIFY:
        respond_msg(ua, t, m, from, 200, 0, NULL); /* unsolicited (e.g. MWI) */
        break;
    case SIP_METHOD_UNKNOWN:
        respond_msg(ua, t, m, from, 501, RF_ALLOW, NULL);
        break;
    default:
        respond_msg(ua, t, m, from, 405, RF_ALLOW, NULL);
        break;
    }
}

/* ------------------------------------------------------------------ */
/* public API                                                           */

static int copy_cfg_str(char *dst, size_t cap, const char *src, bool required)
{
    if (!src || !src[0]) {
        dst[0] = '\0';
        return required ? -1 : 0;
    }
    if (strlen(src) >= cap) {
        return -1;
    }
    strcpy(dst, src);
    return 0;
}

static bool valid_user(const char *s)
{
    for (; *s; s++) {
        if (!sip_is_token_char(*s) || *s == '%') {
            return false;
        }
    }
    return true;
}

static void update_derived(sip_ua_t *ua)
{
    bool dom_is_server = strcmp(ua->domain, ua->server_host) == 0;
    if (dom_is_server && ua->server_port != 5060) {
        snprintf(ua->hostpart, sizeof(ua->hostpart), "%s:%u", ua->domain, (unsigned)ua->server_port);
    } else {
        snprintf(ua->hostpart, sizeof(ua->hostpart), "%s", ua->domain);
    }
    snprintf(ua->registrar, sizeof(ua->registrar), "sip:%s", ua->hostpart);
    sip_wbuf_t w;
    sip_w_init(&w, ua->from_na, sizeof(ua->from_na));
    if (ua->display[0]) {
        sip_w_cstr(&w, "\"");
        for (const char *p = ua->display; *p; p++) {
            if (*p == '"' || *p == '\\') {
                sip_w_cstr(&w, "\\");
            }
            sip_w_mem(&w, p, 1);
        }
        sip_w_cstr(&w, "\" ");
    }
    sip_w_fmt(&w, "<sip:%s@%s>", ua->user, ua->domain);
}

sip_err_t sip_ua_create(const sip_ua_config_t *cfg, sip_ua_t **out)
{
    if (!cfg || !out || !cfg->send || !cfg->rand) {
        return SIP_ERR_ARG;
    }
    *out = NULL;
    sip_ua_t *ua = calloc(1, sizeof(*ua));
    if (!ua) {
        return SIP_ERR_NOMEM;
    }
    if (copy_cfg_str(ua->server_host, sizeof(ua->server_host), cfg->server_host, true) ||
        copy_cfg_str(ua->domain, sizeof(ua->domain),
                     (cfg->domain && cfg->domain[0]) ? cfg->domain : cfg->server_host, true) ||
        copy_cfg_str(ua->user, sizeof(ua->user), cfg->user, true) ||
        copy_cfg_str(ua->auth_user, sizeof(ua->auth_user),
                     (cfg->auth_user && cfg->auth_user[0]) ? cfg->auth_user : cfg->user, true) ||
        copy_cfg_str(ua->password, sizeof(ua->password), cfg->password, false) ||
        copy_cfg_str(ua->display, sizeof(ua->display), cfg->display_name, false) ||
        !valid_user(ua->user)) {
        free_ua(ua);
        return SIP_ERR_ARG;
    }
    for (const char *p = ua->display; *p; p++) {
        if ((unsigned char)*p < 0x20) {
            free_ua(ua);
            return SIP_ERR_ARG;
        }
    }
    ua->server_port = cfg->server_port ? cfg->server_port : 5060;
    ua->server = cfg->server_addr;
    if (ua->server.ip == 0 && !sip_ip_parse(ua->server_host, &ua->server.ip)) {
        free_ua(ua);
        return SIP_ERR_ARG; /* hostnames must be resolved by the caller */
    }
    if (ua->server.port == 0) {
        ua->server.port = ua->server_port;
    }
    ua->expires = cfg->expires ? cfg->expires : 300;
    ua->local = cfg->local_addr;
    ua->contact = ua->local;
    ua->rtp_port = cfg->rtp_port;
    ua->rtp_alloc = cfg->rtp_port_alloc;
    ua->rtp_ctx = cfg->rtp_port_ctx;
    if (cfg->n_codecs == 0) {
        ua->codecs[0] = 9;
        ua->codecs[1] = 0;
        ua->codecs[2] = 8;
        ua->n_codecs = 3;
    } else {
        if (cfg->n_codecs > SIP_UA_MAX_CODECS) {
            free_ua(ua);
            return SIP_ERR_ARG;
        }
        for (int i = 0; i < cfg->n_codecs; i++) {
            uint8_t pt = cfg->codecs[i];
            bool dup = false;
            for (int j = 0; j < i; j++) {
                dup = dup || ua->codecs[j] == pt;
            }
            if ((pt != 0 && pt != 8 && pt != 9) || dup) {
                free_ua(ua);
                return SIP_ERR_ARG;
            }
            ua->codecs[i] = pt;
        }
        ua->n_codecs = cfg->n_codecs;
    }
    ua->dtmf_pt = cfg->dtmf_pt == 0 ? 101 : (cfg->dtmf_pt < 0 ? -1 : cfg->dtmf_pt);
    if (ua->dtmf_pt > 127 || (ua->dtmf_pt >= 0 && ua->dtmf_pt < 96)) {
        free_ua(ua);
        return SIP_ERR_ARG;
    }
    ua->ptime = cfg->ptime_ms ? cfg->ptime_ms : 20;
    ua->keepalive_s = cfg->keepalive_s;
    ua->ka_mode = cfg->keepalive_mode;
    ua->nat_fix = !cfg->disable_nat_contact_fix;
    ua->any_source = cfg->accept_any_source;
    ua->send = cfg->send;
    ua->send_ctx = cfg->send_ctx;
    ua->rand = cfg->rand;
    ua->rand_ctx = cfg->rand_ctx;
    ua->log = cfg->log;
    ua->log_ctx = cfg->log_ctx;
    ua->log_level = cfg->log_level ? cfg->log_level : SIP_LOG_INFO;
    ua->cb = cfg->cb;
    ua->next_handle = 1;
    update_derived(ua);

    sip_txn_tu_t tu = {
        .send = tu_send,
        .on_response = tu_on_response,
        .on_timeout = tu_on_timeout,
        .ctx = ua,
    };
    sip_txn_layer_init(&ua->txl, &tu);
    ua->reg.expires_req = ua->expires;
    new_call_id(ua, ua->reg.call_id);
    new_tag(ua, ua->reg.tag);
    *out = ua;
    return SIP_OK;
}

void sip_ua_destroy(sip_ua_t *ua)
{
    if (!ua) {
        return;
    }
    if (ua->depth > 0) {
        ua->destroy_pending = true;
        return;
    }
    free_ua(ua);
}

void sip_ua_tick(sip_ua_t *ua, uint64_t now_ms)
{
    if (ua->destroy_pending) {
        return;
    }
    enter(ua);
    if (now_ms > ua->now) {
        ua->now = now_ms;
    }
    sip_txn_tick(&ua->txl, ua->now);
    reg_tick(ua);
    for (int i = 0; i < MAX_CALLS; i++) {
        call_t *c = &ua->calls[i];
        if (c->st == CS_ENDING && ua->now >= c->deadline) {
            if (c->bye_pending && c->dlg_valid) {
                send_bye(ua, c);
            }
            call_free(c);
        }
    }
    leave(ua);
}

void sip_ua_on_packet(sip_ua_t *ua, const void *data, size_t len, const sip_addr_t *from)
{
    if (!ua || !data || !from || ua->destroy_pending) {
        return;
    }
    enter(ua);
    const char *p = data;
    size_t i = 0;
    while (i < len && (p[i] == '\r' || p[i] == '\n' || p[i] == ' ')) {
        i++;
    }
    if (i == len) {
        leave(ua); /* keepalive ping/pong (RFC 5626 §4.4.1) */
        return;
    }
    if (!ua->any_source && from->ip != ua->server.ip) {
        LOGD("ignoring datagram from a non-server address");
        leave(ua);
        return;
    }
    if (len > SIP_MAX_MSG_IN) {
        LOGW("dropping oversized datagram (%u bytes)", (unsigned)len);
        leave(ua);
        return;
    }
    memcpy(ua->rx, data, len);
    ua->rx[len] = '\0';
    log_msg(ua, "recv", ua->rx, len, from);
    sip_msg_t *m = &ua->msg;
    if (sip_msg_parse(m, ua->rx, len) != SIP_PARSE_OK) {
        LOGW("dropping malformed message (%u bytes)", (unsigned)len);
        leave(ua);
        return;
    }
    if (m->is_request) {
        on_request(ua, m, from);
    } else if (!sip_txn_on_response(&ua->txl, ua->now, m)) {
        LOGD("stray %d response dropped", m->status);
    }
    leave(ua);
}

sip_err_t sip_ua_register(sip_ua_t *ua)
{
    enter(ua);
    ua->reg.active = true;
    ua->reg.unregistering = false;
    ua->reg.auth_tries = 0;
    ua->reg.backoff_ms = 0;
    ua->reg.t_retry = 0;
    ua->reg.t_refresh = 0;
    if (!ua->reg.txn) {
        if (ua->reg.state != SIP_UA_REG_REGISTERED) {
            reg_set_state(ua, SIP_UA_REG_REGISTERING, 0);
        }
        reg_start(ua);
    }
    leave(ua);
    return SIP_OK;
}

sip_err_t sip_ua_unregister(sip_ua_t *ua)
{
    enter(ua);
    reg_t *r = &ua->reg;
    r->active = false;
    r->t_refresh = 0;
    r->t_retry = 0;
    sip_txn_t *t = sip_txn_find(&ua->txl, r->txn);
    if (t) {
        sip_txn_terminate(t); /* abandon an in-flight REGISTER */
    }
    r->txn = 0;
    if (r->state == SIP_UA_REG_NONE) {
        leave(ua);
        return SIP_OK;
    }
    r->auth_tries = 0;
    r->unregistering = true;
    if (reg_send(ua, 0) != 0) {
        r->unregistering = false;
        reg_set_state(ua, SIP_UA_REG_NONE, 0);
    }
    leave(ua);
    return SIP_OK;
}

sip_ua_reg_state_t sip_ua_reg_state(const sip_ua_t *ua)
{
    return ua->reg.state;
}

sip_err_t sip_ua_set_local_addr(sip_ua_t *ua, const sip_addr_t *local)
{
    if (!local) {
        return SIP_ERR_ARG;
    }
    enter(ua);
    ua->local = *local;
    ua->contact = *local;
    if (ua->reg.active && !ua->reg.txn) {
        reg_start(ua);
    }
    leave(ua);
    return SIP_OK;
}

static int build_ruri(sip_ua_t *ua, const char *number, char *out, size_t cap)
{
    if (strncmp(number, "sip:", 4) == 0 || strncmp(number, "sips:", 5) == 0) {
        sip_uri_t u;
        if (strlen(number) >= cap || sip_uri_parse(sip_str_c(number), &u) != 0) {
            return -1;
        }
        strcpy(out, number);
        return 0;
    }
    /* escape the user part (§25.1: '#' etc. are not allowed unescaped) */
    sip_wbuf_t w;
    sip_w_init(&w, out, cap);
    sip_w_cstr(&w, "sip:");
    if (!number[0]) {
        return -1;
    }
    for (const unsigned char *p = (const unsigned char *)number; *p; p++) {
        if ((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
            strchr("-_.!~*'()&=+$,;?/", *p)) {
            sip_w_mem(&w, p, 1);
        } else if (*p < 0x20 || *p >= 0x7f) {
            return -1;
        } else {
            sip_w_fmt(&w, "%%%02X", *p);
        }
    }
    sip_w_fmt(&w, "@%s", ua->hostpart);
    return w.overflow ? -1 : 0;
}

sip_err_t sip_call_dial(sip_ua_t *ua, const char *number, sip_call_handle_t *out)
{
    if (!number || !out) {
        return SIP_ERR_ARG;
    }
    enter(ua);
    sip_err_t err = SIP_OK;
    call_t *c = NULL;
    if (have_visible_call(ua)) {
        err = SIP_ERR_BUSY;
        goto done;
    }
    c = call_alloc(ua);
    if (!c) {
        err = SIP_ERR_NOMEM;
        goto done;
    }
    if (build_ruri(ua, number, c->ruri, sizeof(c->ruri)) != 0) {
        call_free(c);
        err = SIP_ERR_ARG;
        goto done;
    }
    snprintf(c->to_na, sizeof(c->to_na), "<%s>", c->ruri);
    snprintf(c->number, sizeof(c->number), "%s", number);
    c->outgoing = true;
    new_call_id(ua, c->call_id);
    new_tag(ua, c->local_tag);
    c->invite_cseq = 1;
    c->st = CS_OUT_CALLING;
    err = (sip_err_t)invite_send(ua, c);
    if (err != SIP_OK) {
        call_free(c);
        goto done;
    }
    LOGI("call %d: dialing %s", c->handle, c->ruri);
    emit_state(ua, c);
    *out = c->handle;
done:
    leave(ua);
    return err;
}

sip_err_t sip_call_answer(sip_ua_t *ua, sip_call_handle_t call)
{
    enter(ua);
    sip_err_t err = SIP_OK;
    call_t *c = find_call(ua, call);
    if (!c) {
        err = SIP_ERR_NOT_FOUND;
    } else if (c->st != CS_IN_RINGING || !sip_txn_find(&ua->txl, c->uas_inv_txn)) {
        err = SIP_ERR_STATE;
    } else {
        c->dlg.confirmed = true;
        int r = answer_invite(ua, c, RF_CONTACT | RF_RR | RF_ALLOW);
        if (r < 0) {
            call_respond(ua, c, -r, 0, NULL, 0);
            end_call(ua, c, SIP_END_ERROR, -r, true);
            err = SIP_ERR_STATE;
        } else {
            c->st = CS_CONFIRMED;
            c->last_code = 200;
            LOGI("call %d answered", c->handle);
            emit_state(ua, c);
        }
    }
    leave(ua);
    return err;
}

sip_err_t sip_call_reject(sip_ua_t *ua, sip_call_handle_t call, int code)
{
    if (code < 400 || code > 699) {
        return SIP_ERR_ARG;
    }
    enter(ua);
    sip_err_t err = SIP_OK;
    call_t *c = find_call(ua, call);
    if (!c) {
        err = SIP_ERR_NOT_FOUND;
    } else if (c->st != CS_IN_RINGING) {
        err = SIP_ERR_STATE;
    } else {
        call_respond(ua, c, code, 0, NULL, 0);
        end_call(ua, c, SIP_END_LOCAL, code, true);
    }
    leave(ua);
    return err;
}

sip_err_t sip_call_hangup(sip_ua_t *ua, sip_call_handle_t call)
{
    enter(ua);
    sip_err_t err = SIP_OK;
    call_t *c = find_call(ua, call);
    if (!c) {
        err = SIP_ERR_NOT_FOUND;
        goto done;
    }
    switch (c->st) {
    case CS_OUT_CALLING:
    case CS_OUT_EARLY:
        end_call(ua, c, SIP_END_LOCAL, 0, false);
        if (c->got_provisional) {
            send_cancel(ua, c);
        } else {
            c->cancel_pending = true;
        }
        break;
    case CS_IN_RINGING:
        call_respond(ua, c, 486, 0, NULL, 0);
        end_call(ua, c, SIP_END_LOCAL, 0, true);
        break;
    case CS_CONFIRMED:
        if (c->awaiting_ack) {
            /* §15: don't BYE before the ACK for our 2xx (or its timeout) */
            c->bye_pending = true;
            end_call(ua, c, SIP_END_LOCAL, 0, false);
        } else {
            send_bye(ua, c);
            end_call(ua, c, SIP_END_LOCAL, 0, true);
        }
        break;
    default:
        break; /* already ending */
    }
done:
    leave(ua);
    return err;
}

sip_err_t sip_call_hold(sip_ua_t *ua, sip_call_handle_t call, bool hold)
{
    enter(ua);
    sip_err_t err = SIP_OK;
    call_t *c = find_call(ua, call);
    if (!c) {
        err = SIP_ERR_NOT_FOUND;
    } else if (c->st != CS_CONFIRMED || c->awaiting_ack || c->reinv_txn) {
        err = SIP_ERR_STATE;
    } else if (c->local_hold != hold) {
        c->reinv_hold = hold;
        c->reinv_auth_tries = 0;
        int n = make_sdp(ua, c, NULL, NULL, hold ? SIP_DIR_SENDONLY : SIP_DIR_SENDRECV);
        if (n < 0) {
            err = SIP_ERR_OVERFLOW;
        } else {
            send_in_dialog(ua, c, "INVITE", SIP_METHOD_INVITE, OWN_REINVITE, ua->sdp, (size_t)n, true);
            err = c->reinv_txn ? SIP_OK : SIP_ERR_NOMEM;
            LOGI("call %d: %s", c->handle, hold ? "hold" : "resume");
        }
    }
    leave(ua);
    return err;
}

sip_ua_call_state_t sip_call_state(const sip_ua_t *ua, sip_call_handle_t call)
{
    for (int i = 0; i < MAX_CALLS; i++) {
        const call_t *c = &ua->calls[i];
        if (c->st != CS_FREE && c->handle == call) {
            return public_state(c);
        }
    }
    return SIP_UA_CALL_IDLE;
}

int sip_ua_debug_txn_count(const sip_ua_t *ua)
{
    return sip_txn_count(&ua->txl);
}

int sip_ua_debug_call_slots(const sip_ua_t *ua)
{
    int n = 0;
    for (int i = 0; i < MAX_CALLS; i++) {
        n += ua->calls[i].st != CS_FREE;
    }
    return n;
}

sip_addr_t sip_ua_debug_contact(const sip_ua_t *ua)
{
    return ua->contact;
}
