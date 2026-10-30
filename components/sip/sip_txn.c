/* SIP transaction layer (RFC 3261 §17, RFC 6026). See sip_txn.h. */

#include "sip_txn.h"

#include <stdio.h>
#include <string.h>

#define MAGIC_COOKIE "z9hG4bK"

void sip_txn_layer_init(sip_txn_layer_t *l, const sip_txn_tu_t *tu)
{
    memset(l, 0, sizeof(*l));
    l->tu = *tu;
    l->next_id = 1;
}

sip_txn_t *sip_txn_find(sip_txn_layer_t *l, uint32_t id)
{
    if (id == 0) {
        return NULL;
    }
    for (int i = 0; i < SIP_MAX_TXN; i++) {
        if (l->pool[i].id == id) {
            return &l->pool[i];
        }
    }
    return NULL;
}

int sip_txn_count(const sip_txn_layer_t *l)
{
    int n = 0;
    for (int i = 0; i < SIP_MAX_TXN; i++) {
        n += l->pool[i].id != 0;
    }
    return n;
}

void sip_txn_terminate(sip_txn_t *t)
{
    t->id = 0;
    t->kind = SIP_TXN_FREE;
}

/* A transaction that has delivered its final outcome and only lingers to
 * absorb retransmissions (timers D, I, J, K, or a client INVITE in Accepted,
 * RFC 6026). Dropping one early just means a late retransmission goes
 * unanswered by the transaction layer. Server INVITEs waiting for an ACK are
 * excluded: they still retransmit their final response. */
static bool lingering(const sip_txn_t *t)
{
    switch (t->kind) {
    case SIP_TXN_CLIENT_INVITE:
        return t->state == SIP_TS_COMPLETED || t->state == SIP_TS_ACCEPTED;
    case SIP_TXN_CLIENT_NONINVITE:
    case SIP_TXN_SERVER_NONINVITE:
        return t->state == SIP_TS_COMPLETED;
    case SIP_TXN_SERVER_INVITE:
        return t->state == SIP_TS_CONFIRMED;
    default:
        return false;
    }
}

static sip_txn_t *alloc_txn(sip_txn_layer_t *l)
{
    sip_txn_t *t = NULL;
    sip_txn_t *victim = NULL;
    for (int i = 0; i < SIP_MAX_TXN && !t; i++) {
        if (l->pool[i].id == 0) {
            t = &l->pool[i];
        } else if (lingering(&l->pool[i]) && (!victim || l->pool[i].t_end < victim->t_end)) {
            victim = &l->pool[i];
        }
    }
    if (!t && victim) {
        /* Pool full (e.g. a burst of short calls): reuse the lingering
         * transaction that would have ended soonest. */
        sip_txn_terminate(victim);
        t = victim;
    }
    if (!t) {
        return NULL;
    }
    memset(t, 0, offsetof(sip_txn_t, buf));
    t->id = l->next_id++;
    if (l->next_id == 0) {
        l->next_id = 1;
    }
    return t;
}

static void txn_send(sip_txn_layer_t *l, sip_txn_t *t)
{
    l->tu.send(l->tu.ctx, &t->peer, t->buf, t->len);
}

static bool store(sip_txn_t *t, const char *buf, size_t len)
{
    if (len > sizeof(t->buf)) {
        return false;
    }
    memcpy(t->buf, buf, len);
    t->len = (uint16_t)len;
    return true;
}

int sip_via_sent_by(const sip_via_t *v, char *out, size_t cap)
{
    int n = snprintf(out, cap, "%.*s:%d", (int)v->host.len, v->host.p, v->port ? v->port : 5060);
    if (n < 0 || (size_t)n >= cap) {
        out[0] = '\0';
        return -1;
    }
    for (int i = 0; i < n; i++) {
        if (out[i] >= 'A' && out[i] <= 'Z') {
            out[i] = (char)(out[i] + 32); /* host comparison is case-insensitive */
        }
    }
    return n;
}

/* ------------------------------------------------------------------ */
/* client                                                               */

sip_txn_t *sip_txn_client_start(sip_txn_layer_t *l, uint64_t now, sip_method_t method,
                                const char *branch, uint32_t cseq, const sip_addr_t *peer,
                                const char *buf, size_t len)
{
    if (strlen(branch) >= SIP_BRANCH_MAX || len > SIP_MAX_MSG_OUT) {
        return NULL;
    }
    sip_txn_t *t = alloc_txn(l);
    if (!t) {
        return NULL;
    }
    bool invite = method == SIP_METHOD_INVITE;
    t->kind = invite ? SIP_TXN_CLIENT_INVITE : SIP_TXN_CLIENT_NONINVITE;
    t->state = invite ? SIP_TS_CALLING : SIP_TS_TRYING;
    t->method = method;
    strcpy(t->branch, branch);
    t->cseq = cseq;
    t->peer = *peer;
    store(t, buf, len);
    /* timer A / E = T1; timer B / F = 64*T1 */
    t->retx_iv = SIP_T1_MS;
    t->t_retx = now + SIP_T1_MS;
    t->t_end = now + 64u * SIP_T1_MS;
    txn_send(l, t);
    return t;
}

/* Copy the INVITE we sent into an ACK or CANCEL (§17.1.1.3, §9.1): same
 * Request-URI, top Via, From, Call-ID, Route; CSeq number unchanged. */
static int build_from_invite(const char *inv, size_t inv_len, const char *method, uint32_t cseq,
                             sip_str_t to_override, char *out, size_t cap)
{
    sip_wbuf_t w;
    sip_w_init(&w, out, cap);
    const char *p = inv;
    const char *e = inv + inv_len;
    bool first = true;
    bool via_done = false;
    while (p < e) {
        const char *nl = memchr(p, '\n', (size_t)(e - p));
        if (!nl) {
            break;
        }
        sip_str_t line = { p, (size_t)(nl - p) };
        if (line.len && line.p[line.len - 1] == '\r') {
            line.len--;
        }
        p = nl + 1;
        if (line.len == 0) {
            break;
        }
        if (first) {
            const char *sp = memchr(line.p, ' ', line.len);
            if (!sp) {
                return -1;
            }
            sip_w_fmt(&w, "%s%.*s\r\n", method, (int)(line.p + line.len - sp), sp);
            first = false;
            continue;
        }
#define HAS_PREFIX(s) (line.len >= sizeof(s) - 1 && memcmp(line.p, s, sizeof(s) - 1) == 0)
        if (HAS_PREFIX("Via:")) {
            if (!via_done) {
                sip_w_str(&w, line);
                sip_w_cstr(&w, "\r\n");
                via_done = true;
            }
        } else if (HAS_PREFIX("From:") || HAS_PREFIX("Call-ID:") || HAS_PREFIX("Route:") ||
                   HAS_PREFIX("Max-Forwards:")) {
            sip_w_str(&w, line);
            sip_w_cstr(&w, "\r\n");
        } else if (HAS_PREFIX("To:")) {
            if (to_override.p) {
                sip_w_cstr(&w, "To: ");
                sip_w_str(&w, to_override);
                sip_w_cstr(&w, "\r\n");
            } else {
                sip_w_str(&w, line);
                sip_w_cstr(&w, "\r\n");
            }
        }
#undef HAS_PREFIX
    }
    sip_w_fmt(&w, "CSeq: %lu %s\r\nUser-Agent: " SIP_UA_USER_AGENT "\r\nContent-Length: 0\r\n\r\n",
              (unsigned long)cseq, method);
    return w.overflow || first ? -1 : (int)w.len;
}

int sip_txn_build_cancel(const sip_txn_t *t, char *out, size_t cap)
{
    if (t->kind != SIP_TXN_CLIENT_INVITE) {
        return -1;
    }
    return build_from_invite(t->buf, t->len, "CANCEL", t->cseq, SIP_STR_NULL, out, cap);
}

static sip_txn_t *client_match(sip_txn_layer_t *l, const sip_msg_t *m)
{
    const sip_str_t br = m->via[0].branch;
    if (br.len == 0) {
        return NULL;
    }
    for (int i = 0; i < SIP_MAX_TXN; i++) {
        sip_txn_t *t = &l->pool[i];
        if ((t->kind == SIP_TXN_CLIENT_INVITE || t->kind == SIP_TXN_CLIENT_NONINVITE) &&
            t->method == m->cseq_method && sip_str_eq(br, sip_str_c(t->branch))) {
            return t;
        }
    }
    return NULL;
}

bool sip_txn_on_response(sip_txn_layer_t *l, uint64_t now, const sip_msg_t *m)
{
    sip_txn_t *t = client_match(l, m);
    if (!t) {
        return false;
    }
    int code = m->status;
    if (t->kind == SIP_TXN_CLIENT_INVITE) {
        switch (t->state) {
        case SIP_TS_CALLING:
        case SIP_TS_PROCEEDING:
            t->last_status = code;
            if (code < 200) {
                t->state = SIP_TS_PROCEEDING;
                t->t_retx = 0;  /* stop timer A */
                t->t_end = 0;   /* and B */
            } else if (code < 300) {
                t->state = SIP_TS_ACCEPTED;
                t->t_retx = 0;
                t->t_end = now + 64u * SIP_T1_MS; /* timer M */
            } else {
                /* the transaction itself ACKs non-2xx finals */
                int n = build_from_invite(t->buf, t->len, "ACK", t->cseq, m->to.raw,
                                          l->scratch, sizeof(l->scratch));
                if (n > 0) {
                    store(t, l->scratch, (size_t)n);
                    txn_send(l, t);
                } else {
                    t->len = 0;
                }
                t->state = SIP_TS_COMPLETED;
                t->t_retx = 0;
                t->t_end = now + SIP_TIMER_D_MS;
            }
            l->tu.on_response(l->tu.ctx, t, m);
            return true;
        case SIP_TS_COMPLETED:
            if (code >= 300 && t->len) {
                txn_send(l, t); /* retransmitted final: resend ACK, absorb */
            }
            return true;
        case SIP_TS_ACCEPTED:
            if (code >= 200 && code < 300) {
                l->tu.on_response(l->tu.ctx, t, m); /* TU re-sends its ACK */
            }
            return true;
        default:
            return true;
        }
    }
    /* non-INVITE */
    switch (t->state) {
    case SIP_TS_TRYING:
    case SIP_TS_PROCEEDING:
        t->last_status = code;
        if (code < 200) {
            t->state = SIP_TS_PROCEEDING;
        } else {
            t->state = SIP_TS_COMPLETED;
            t->t_retx = 0;
            t->t_end = now + SIP_T4_MS; /* timer K */
        }
        l->tu.on_response(l->tu.ctx, t, m);
        return true;
    default:
        return true; /* Completed: absorb */
    }
}

/* ------------------------------------------------------------------ */
/* server                                                               */

sip_txn_t *sip_txn_server_match(sip_txn_layer_t *l, const sip_msg_t *m)
{
    const sip_via_t *v = &m->via[0];
    if (v->branch.len < 7 || memcmp(v->branch.p, MAGIC_COOKIE, 7) != 0) {
        return NULL; /* RFC 2543 matching not supported */
    }
    char sb[SIP_SENTBY_MAX];
    if (sip_via_sent_by(v, sb, sizeof(sb)) < 0) {
        return NULL;
    }
    sip_method_t want = m->method == SIP_METHOD_ACK ? SIP_METHOD_INVITE : m->method;
    for (int i = 0; i < SIP_MAX_TXN; i++) {
        sip_txn_t *t = &l->pool[i];
        if ((t->kind == SIP_TXN_SERVER_INVITE || t->kind == SIP_TXN_SERVER_NONINVITE) &&
            t->method == want && sip_str_eq(v->branch, sip_str_c(t->branch)) &&
            strcmp(sb, t->sent_by) == 0) {
            return t;
        }
    }
    return NULL;
}

bool sip_txn_server_absorb(sip_txn_layer_t *l, uint64_t now, sip_txn_t *t, const sip_msg_t *m)
{
    if (m->method == SIP_METHOD_ACK) {
        if (t->kind == SIP_TXN_SERVER_INVITE && t->state == SIP_TS_COMPLETED) {
            t->state = SIP_TS_CONFIRMED;
            t->t_retx = 0;
            t->t_end = now + SIP_T4_MS; /* timer I */
            return true;
        }
        if (t->kind == SIP_TXN_SERVER_INVITE && t->state == SIP_TS_CONFIRMED) {
            return true;
        }
        /* ACK for a 2xx shares no branch with the INVITE (§13.2.2.4) unless the
         * peer reuses it; let the TU see it. */
        return false;
    }
    /* retransmitted request: resend the last response, if any */
    if (t->len && t->state != SIP_TS_CONFIRMED) {
        txn_send(l, t);
    }
    return true;
}

sip_txn_t *sip_txn_server_create(sip_txn_layer_t *l, uint64_t now, const sip_msg_t *m,
                                 const sip_addr_t *from)
{
    (void)now;
    const sip_via_t *v = &m->via[0];
    if (v->branch.len >= SIP_BRANCH_MAX) {
        return NULL;
    }
    sip_txn_t *t = alloc_txn(l);
    if (!t) {
        return NULL;
    }
    bool invite = m->method == SIP_METHOD_INVITE;
    t->kind = invite ? SIP_TXN_SERVER_INVITE : SIP_TXN_SERVER_NONINVITE;
    t->state = invite ? SIP_TS_PROCEEDING : SIP_TS_TRYING;
    t->method = m->method;
    sip_str_copy(t->branch, sizeof(t->branch), v->branch);
    if (sip_via_sent_by(v, t->sent_by, sizeof(t->sent_by)) < 0) {
        sip_txn_terminate(t);
        return NULL;
    }
    t->cseq = m->cseq;
    /* responses go back to the source address (RFC 3581 symmetric response) */
    t->peer = *from;
    t->len = 0;
    return t;
}

void sip_txn_respond(sip_txn_layer_t *l, uint64_t now, sip_txn_t *t, int status,
                     const char *buf, size_t len)
{
    if (!store(t, buf, len)) {
        return;
    }
    t->last_status = status;
    if (t->kind == SIP_TXN_SERVER_INVITE) {
        if (status < 200) {
            t->state = SIP_TS_PROCEEDING;
        } else if (status < 300) {
            /* 2xx retransmission by the UAS core, §13.3.1.4 */
            t->state = SIP_TS_ACCEPTED;
            t->retx_iv = SIP_T1_MS;
            t->t_retx = now + SIP_T1_MS;
            t->t_end = now + 64u * SIP_T1_MS;
        } else {
            t->state = SIP_TS_COMPLETED;
            t->retx_iv = SIP_T1_MS;
            t->t_retx = now + SIP_T1_MS;      /* timer G */
            t->t_end = now + 64u * SIP_T1_MS; /* timer H */
        }
    } else {
        if (status < 200) {
            t->state = SIP_TS_PROCEEDING;
        } else {
            t->state = SIP_TS_COMPLETED;
            t->t_end = now + 64u * SIP_T1_MS; /* timer J */
        }
    }
    txn_send(l, t);
}

void sip_txn_ack_2xx(sip_txn_t *t)
{
    if (t->kind == SIP_TXN_SERVER_INVITE && t->state == SIP_TS_ACCEPTED) {
        /* stop retransmitting; linger to absorb INVITE retransmissions */
        t->t_retx = 0;
        t->state = SIP_TS_CONFIRMED;
    }
}

/* ------------------------------------------------------------------ */
/* timers                                                               */

uint64_t sip_txn_next_deadline(const sip_txn_layer_t *l)
{
    uint64_t d = UINT64_MAX;
    for (int i = 0; i < SIP_MAX_TXN; i++) {
        const sip_txn_t *t = &l->pool[i];
        if (t->id == 0) {
            continue;
        }
        if (t->t_retx && t->t_retx < d) {
            d = t->t_retx;
        }
        if (t->t_end && t->t_end < d) {
            d = t->t_end;
        }
    }
    return d;
}

void sip_txn_tick(sip_txn_layer_t *l, uint64_t now)
{
    for (int i = 0; i < SIP_MAX_TXN; i++) {
        sip_txn_t *t = &l->pool[i];
        if (t->id == 0) {
            continue;
        }
        if (t->t_retx && now >= t->t_retx) {
            txn_send(l, t);
            /* INVITE client timer A doubles without cap; E, G and the 2xx
             * retransmission are capped at T2 (§17.1.2.2, §17.2.1). */
            uint32_t iv = t->retx_iv * 2;
            if (t->kind != SIP_TXN_CLIENT_INVITE && iv > SIP_T2_MS) {
                iv = SIP_T2_MS;
            }
            if (t->kind == SIP_TXN_CLIENT_NONINVITE && t->state == SIP_TS_PROCEEDING) {
                iv = SIP_T2_MS;
            }
            t->retx_iv = iv;
            t->t_retx = now + iv;
        }
        if (t->t_end && now >= t->t_end) {
            bool notify = false;
            switch (t->kind) {
            case SIP_TXN_CLIENT_INVITE:
                notify = t->state == SIP_TS_CALLING;          /* timer B */
                break;
            case SIP_TXN_CLIENT_NONINVITE:
                notify = t->state == SIP_TS_TRYING || t->state == SIP_TS_PROCEEDING; /* F */
                break;
            case SIP_TXN_SERVER_INVITE:
                notify = t->state == SIP_TS_COMPLETED || t->state == SIP_TS_ACCEPTED; /* H */
                break;
            default:
                break;
            }
            uint32_t id = t->id;
            if (notify) {
                l->tu.on_timeout(l->tu.ctx, t);
            }
            /* the TU may have terminated (and even reused) the slot */
            if (t->id == id) {
                sip_txn_terminate(t);
            }
        }
    }
}
