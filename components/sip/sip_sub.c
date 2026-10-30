/* Subscriber side of RFC 6665: SUBSCRIBE dialogs for the "dialog" (BLF,
 * RFC 4235) and "message-summary" (MWI, RFC 3842) packages, NOTIFY handling
 * and unsolicited message-summary NOTIFYs. Part of the UA (sip_ua_priv.h).
 *
 * Life of a subscription slot:
 *   WAIT --(registered, retry time, < SUB_MAX_INFLIGHT in flight)--> TRYING
 *   TRYING --2xx / NOTIFY--> ACTIVE (dialog) --refresh before expiry--> ACTIVE
 *   TRYING/ACTIVE --failure / terminated--> WAIT (backoff, retry-after)
 *   unsubscribe: WAIT/TRYING → FREE; ACTIVE → ENDING (Expires: 0) → FREE
 * A NOTIFY that arrives before the 2xx establishes the dialog (§4.1.2.4). */

#include <stdio.h>
#include <string.h>

#include "sip_ua_internal.h"
#include "sip_ua_priv.h"

#define SUB_BACKOFF_MIN    5000u     /* first retry after a soft failure */
#define SUB_BACKOFF_HARD   60000u    /* first retry after 403/404/489/rejected/noresource */
#define SUB_BACKOFF_MAX    300000u
#define SUB_RETRY_AFTER_MAX 3600u    /* cap for a server's retry-after (s) */
#define SUB_MIN_LIFETIME   30000u    /* ended sooner: back off instead of retrying at once */
#define SUB_NOTXN_RETRY    1000u     /* transaction pool full: try again shortly */

static const char *kind_name(const sub_t *s)
{
    return s->kind == SUB_KIND_BLF ? "blf" : "mwi";
}

static const char *blf_name(sip_blf_state_t st)
{
    switch (st) {
    case SIP_BLF_IDLE:    return "idle";
    case SIP_BLF_RINGING: return "ringing";
    case SIP_BLF_BUSY:    return "busy";
    default:              return "unknown";
    }
}

static sub_t *sub_find(sip_ua_t *ua, sip_sub_handle_t h)
{
    if (h <= 0) {
        return NULL;
    }
    for (int i = 0; i < SIP_UA_MAX_SUBS; i++) {
        if (ua->subs[i].st != SUB_FREE && ua->subs[i].handle == h) {
            return &ua->subs[i];
        }
    }
    return NULL;
}

static int inflight(const sip_ua_t *ua)
{
    int n = 0;
    for (int i = 0; i < SIP_UA_MAX_SUBS; i++) {
        n += ua->subs[i].st != SUB_FREE && ua->subs[i].txn != 0;
    }
    return n;
}

static void sub_free(sub_t *s)
{
    s->st = SUB_FREE;
    s->handle = 0;
    s->txn = 0;
}

static void report_blf(sip_ua_t *ua, sub_t *s, sip_blf_state_t st)
{
    if (s->kind != SUB_KIND_BLF || !s->want || s->reported == (int)st) {
        return;
    }
    s->reported = (int)st;
    LOGI("blf %s: %s", s->target, blf_name(st));
    ev_t *e = sipua_ev_push(ua, EV_BLF, s->handle);
    if (e) {
        e->state = (int)st;
        snprintf(e->number, sizeof(e->number), "%s", s->target);
    }
}

static void report_mwi(sip_ua_t *ua, const sip_msgsum_t *ms)
{
    if (ua->mwi_reported && ua->mwi_waiting == ms->waiting && ua->mwi_new == ms->new_msgs &&
        ua->mwi_old == ms->old_msgs) {
        return;
    }
    ua->mwi_reported = true;
    ua->mwi_waiting = ms->waiting;
    ua->mwi_new = ms->new_msgs;
    ua->mwi_old = ms->old_msgs;
    LOGI("mwi: %s, %lu new / %lu old", ms->waiting ? "waiting" : "none", (unsigned long)ms->new_msgs,
         (unsigned long)ms->old_msgs);
    ev_t *e = sipua_ev_push(ua, EV_MWI, 0);
    if (e) {
        e->state = ms->waiting;
        e->reason = (int)ms->new_msgs;
        e->code = (int)ms->old_msgs;
    }
}

/* ------------------------------------------------------------------ */
/* BLF state from dialog-info                                           */

static sip_blf_state_t blf_from_dialogs(const sub_t *s)
{
    bool ringing = false, busy = false;
    for (int i = 0; i < s->n_dlg; i++) {
        const sub_dlg_t *d = &s->dlg_state[i];
        switch ((sip_dlg_state_t)d->state) {
        case SIP_DLG_EARLY:
        case SIP_DLG_TRYING:
        case SIP_DLG_PROCEEDING:
            /* being called rings; placing a call (initiator) means in use */
            if (d->dir == SIP_DLGDIR_INITIATOR) {
                busy = true;
            } else {
                ringing = true;
            }
            break;
        case SIP_DLG_TERMINATED:
            break;
        case SIP_DLG_CONFIRMED:
        case SIP_DLG_UNKNOWN:
        default:
            busy = true;
            break;
        }
    }
    return ringing ? SIP_BLF_RINGING : busy ? SIP_BLF_BUSY : SIP_BLF_IDLE;
}

static void dlg_upsert(sub_t *s, const sip_dlginfo_dialog_t *d)
{
    int i;
    for (i = 0; i < s->n_dlg; i++) {
        if (strncmp(s->dlg_state[i].id, d->id, sizeof(s->dlg_state[i].id) - 1) == 0) {
            break;
        }
    }
    if (d->state == SIP_DLG_TERMINATED) {
        if (i < s->n_dlg) {
            s->dlg_state[i] = s->dlg_state[s->n_dlg - 1];
            s->n_dlg--;
        }
        return;
    }
    if (i == s->n_dlg) {
        if (s->n_dlg == SUB_DLG_MAX) {
            return; /* too many concurrent dialogs: the lamp is busy anyway */
        }
        s->n_dlg++;
    }
    size_t n = 0;
    while (n + 1 < sizeof(s->dlg_state[i].id) && d->id[n]) {
        n++; /* ids longer than ours are truncated alike when they come back */
    }
    memcpy(s->dlg_state[i].id, d->id, n);
    s->dlg_state[i].id[n] = '\0';
    s->dlg_state[i].state = (uint8_t)d->state;
    s->dlg_state[i].dir = (uint8_t)d->dir;
}

static void apply_dlginfo(sip_ua_t *ua, sub_t *s, const sip_msg_t *m)
{
    if (m->body.len == 0) {
        return; /* e.g. a pending subscription: no state yet */
    }
    if (!sip_content_type_is(m->content_type, "application/dialog-info+xml")) {
        LOGW("blf %s: unexpected NOTIFY body type", s->target);
        return;
    }
    sip_dlginfo_t di; /* ~0.5 KB */
    if (sip_dlginfo_parse(m->body.p, m->body.len, &di) != 0) {
        LOGW("blf %s: malformed dialog-info ignored", s->target);
        return;
    }
    if (s->have_version && di.version <= s->version) {
        LOGD("blf %s: stale dialog-info version %lu ignored", s->target, (unsigned long)di.version);
        return;
    }
    if (!di.full && (!s->have_version || di.version != s->version + 1)) {
        s->refresh_now = true; /* lost a partial update: a refresh brings full state */
    }
    s->have_version = true;
    s->version = di.version;
    if (di.full) {
        s->n_dlg = 0;
    }
    for (int i = 0; i < di.n; i++) {
        dlg_upsert(s, &di.d[i]);
    }
    report_blf(ua, s, blf_from_dialogs(s));
}

static void apply_msgsum(sip_ua_t *ua, const sip_msg_t *m)
{
    if (m->body.len == 0) {
        return;
    }
    if (!sip_content_type_is(m->content_type, "application/simple-message-summary")) {
        LOGW("mwi: unexpected NOTIFY body type");
        return;
    }
    sip_msgsum_t ms;
    if (sip_msgsum_parse(m->body.p, m->body.len, &ms) != 0) {
        LOGW("mwi: malformed message-summary ignored");
        return;
    }
    report_mwi(ua, &ms);
}

/* ------------------------------------------------------------------ */
/* sending                                                              */

static bool sub_send(sip_ua_t *ua, sub_t *s, uint32_t expires)
{
    char branch[32], sb[24];
    sip_wbuf_t w;
    uint32_t cseq;
    const char *auth_uri;
    sipua_new_branch(ua, branch);
    sip_w_init(&w, ua->tx, sizeof(ua->tx));
    if (s->dlg_valid) {
        sipua_sent_by(ua, sb);
        cseq = ++s->dlg.local_cseq;
        sip_dialog_write_request(&s->dlg, &w, "SUBSCRIBE", cseq, sb, branch);
        auth_uri = s->dlg.remote_target;
    } else {
        cseq = ++s->cseq;
        sip_w_fmt(&w, "SUBSCRIBE %s SIP/2.0\r\n", s->ruri);
        sipua_w_via(ua, &w, branch);
        sip_w_cstr(&w, "Max-Forwards: 70\r\n");
        sip_w_fmt(&w, "From: %s;tag=%s\r\n", ua->from_na, s->local_tag);
        sip_w_fmt(&w, "To: <%s>\r\n", s->ruri);
        sip_w_fmt(&w, "Call-ID: %s\r\n", s->call_id);
        sip_w_fmt(&w, "CSeq: %lu SUBSCRIBE\r\n", (unsigned long)cseq);
        auth_uri = s->ruri;
    }
    sipua_w_contact(ua, &w);
    if (s->kind == SUB_KIND_BLF) {
        sip_w_cstr(&w, "Event: dialog\r\nAccept: application/dialog-info+xml\r\n");
    } else {
        sip_w_cstr(&w, "Event: message-summary\r\nAccept: application/simple-message-summary\r\n");
    }
    sip_w_fmt(&w, "Expires: %lu\r\n", (unsigned long)expires);
    sip_w_cstr(&w, ALLOW_HDR UA_HDR);
    sipua_w_auth(ua, &w, "SUBSCRIBE", auth_uri);
    sip_w_cstr(&w, "Content-Length: 0\r\n\r\n");
    if (w.overflow) {
        LOGE("%s %s: SUBSCRIBE does not fit", kind_name(s), s->target);
        return false;
    }
    sip_txn_t *t = sip_txn_client_start(&ua->txl, ua->now, SIP_METHOD_SUBSCRIBE, branch, cseq, &ua->server,
                                        ua->tx, w.len);
    if (!t) {
        LOGW("%s %s: no free transaction for SUBSCRIBE", kind_name(s), s->target);
        return false;
    }
    sipua_log_msg(ua, "send", ua->tx, w.len, &ua->server);
    t->owner = OWN_SUBSCRIBE;
    t->owner_ref = s->handle;
    s->txn = t->id;
    s->txn_expires = expires;
    s->txn_in_dialog = s->dlg_valid;
    return true;
}

/* New subscription (new Call-ID and tag): from WAIT to TRYING. */
static void sub_start(sip_ua_t *ua, sub_t *s)
{
    sipua_new_call_id(ua, s->call_id);
    sipua_new_tag(ua, s->local_tag);
    s->cseq = 0;
    s->dlg_valid = false;
    s->have_version = false;
    s->n_dlg = 0;
    s->auth_tries = 0;
    s->interval_tries = 0;
    s->refresh_now = false;
    s->t_started = ua->now;
    if (!sub_send(ua, s, s->expires_req)) {
        s->st = SUB_WAIT;
        s->t_retry = ua->now + SUB_NOTXN_RETRY;
        return;
    }
    s->st = SUB_TRYING;
    LOGD("%s %s: SUBSCRIBE sent", kind_name(s), s->target);
}

/* Not subscribed any more: retry after `wait_ms` (0 = as soon as possible). */
static void sub_wait(sub_t *s, uint64_t now, uint32_t wait_ms)
{
    s->st = SUB_WAIT;
    s->dlg_valid = false;
    s->txn = 0;
    s->t_retry = now + wait_ms;
}

static uint32_t next_backoff(sub_t *s, bool hard)
{
    uint32_t b = s->backoff_ms ? s->backoff_ms * 2 : (hard ? SUB_BACKOFF_HARD : SUB_BACKOFF_MIN);
    if (hard && b < SUB_BACKOFF_HARD) {
        b = SUB_BACKOFF_HARD;
    }
    if (b > SUB_BACKOFF_MAX) {
        b = SUB_BACKOFF_MAX;
    }
    s->backoff_ms = b;
    return b;
}

/* The subscription failed / ended for good: report UNKNOWN, retry later. */
static void sub_fail(sip_ua_t *ua, sub_t *s, int code, bool hard, int retry_after_s)
{
    uint32_t wait;
    if (retry_after_s > 0) {
        wait = (uint32_t)(retry_after_s > (int)SUB_RETRY_AFTER_MAX ? SUB_RETRY_AFTER_MAX : (uint32_t)retry_after_s) *
               1000u;
        next_backoff(s, hard); /* still escalate if it keeps failing */
    } else {
        wait = next_backoff(s, hard);
    }
    LOGW("%s %s: subscription failed (%d), retry in %lu s", kind_name(s), s->target, code,
         (unsigned long)(wait / 1000));
    sub_wait(s, ua->now, wait);
    report_blf(ua, s, SIP_BLF_UNKNOWN);
}

static void send_unsubscribe(sip_ua_t *ua, sub_t *s)
{
    sip_txn_t *t = sip_txn_find(&ua->txl, s->txn);
    if (t) {
        sip_txn_terminate(t); /* abandon a refresh in flight */
    }
    s->txn = 0;
    s->auth_tries = 0;
    if (s->dlg_valid && sub_send(ua, s, 0)) {
        s->st = SUB_ENDING;
        LOGD("%s %s: un-SUBSCRIBE sent", kind_name(s), s->target);
    } else {
        sub_free(s);
    }
}

/* ------------------------------------------------------------------ */
/* transaction results                                                  */

void sipua_sub_on_response(sip_ua_t *ua, sip_txn_t *t, const sip_msg_t *m)
{
    sub_t *s = sub_find(ua, t->owner_ref);
    int code = m->status;
    if (!s || t->id != s->txn || code < 200) {
        return;
    }
    s->txn = 0;
    if ((code == 401 || code == 407) && sipua_auth_retry(ua, &s->auth_tries, m)) {
        if (sub_send(ua, s, s->txn_expires)) {
            return;
        }
        if (s->st == SUB_ENDING) {
            sub_free(s);
        } else {
            sub_fail(ua, s, code, false, -1);
        }
        return;
    }
    s->auth_tries = 0;
    if (s->st == SUB_ENDING) {
        LOGD("%s %s: un-SUBSCRIBE answered %d", kind_name(s), s->target, code);
        sub_free(s);
        return;
    }
    if (code == 423 && m->min_expires > 0 && s->interval_tries < 2) {
        s->interval_tries++;
        s->expires_req = (uint32_t)m->min_expires;
        if (!sub_send(ua, s, s->expires_req)) {
            sub_fail(ua, s, code, false, -1);
        }
        return;
    }
    s->interval_tries = 0;
    if (code < 300) {
        if (!s->dlg_valid) {
            char to_na[SIP_URI_MAX + 2];
            snprintf(to_na, sizeof(to_na), "<%s>", s->ruri);
            if (sip_dialog_init_uac(&s->dlg, m, s->call_id, ua->from_na, s->local_tag, to_na, s->ruri,
                                    s->cseq) != 0) {
                sub_fail(ua, s, 500, false, -1);
                return;
            }
            s->dlg.confirmed = true;
            s->dlg_valid = true;
        } else if (m->n_contact > 0) {
            sip_dialog_set_target(&s->dlg, m);
        }
        uint32_t granted = m->expires >= 0 ? (uint32_t)m->expires : s->txn_expires;
        if (granted == 0) {
            sub_fail(ua, s, code, false, -1); /* 2xx with Expires: 0: not subscribed */
            return;
        }
        if (s->st != SUB_ACTIVE) {
            LOGI("%s %s: subscribed, expires %lu s", kind_name(s), s->target, (unsigned long)granted);
        }
        s->st = SUB_ACTIVE;
        s->t_refresh = ua->now + (uint64_t)sipua_refresh_delay_s(granted) * 1000u;
        s->t_expire = ua->now + (uint64_t)granted * 1000u;
        if (!s->want) {
            send_unsubscribe(ua, s);
        }
        return;
    }
    if (s->txn_in_dialog && (code == 481 || code == 408)) {
        /* the notifier forgot the subscription: start a new one right away */
        LOGI("%s %s: refresh got %d, re-subscribing", kind_name(s), s->target, code);
        sub_wait(s, ua->now, 0);
        return;
    }
    bool hard = code == 403 || code == 404 || code == 405 || code == 489 || code == 501 || code == 603 ||
                code == 604;
    sub_fail(ua, s, code, hard, m->retry_after);
}

void sipua_sub_on_timeout(sip_ua_t *ua, sip_txn_t *t)
{
    sub_t *s = sub_find(ua, t->owner_ref);
    if (!s || t->id != s->txn) {
        return;
    }
    s->txn = 0;
    if (s->st == SUB_ENDING) {
        sub_free(s);
        return;
    }
    sub_fail(ua, s, 408, false, -1);
}

/* ------------------------------------------------------------------ */
/* NOTIFY                                                               */

static sub_t *sub_for_notify(sip_ua_t *ua, const sip_msg_t *m)
{
    for (int i = 0; i < SIP_UA_MAX_SUBS; i++) {
        sub_t *s = &ua->subs[i];
        if (s->st != SUB_TRYING && s->st != SUB_ACTIVE && s->st != SUB_ENDING) {
            continue;
        }
        const char *cid = s->dlg_valid ? s->dlg.call_id : s->call_id;
        const char *ltag = s->dlg_valid ? s->dlg.local_tag : s->local_tag;
        if (!sip_str_eq(m->call_id, sip_str_c(cid)) || !sip_str_eq(m->to.tag, sip_str_c(ltag))) {
            continue;
        }
        if (s->dlg_valid && s->dlg.remote_tag[0] && !sip_str_eq(m->from.tag, sip_str_c(s->dlg.remote_tag))) {
            continue; /* another (forked) dialog: not supported */
        }
        return s;
    }
    return NULL;
}

static void on_terminated(sip_ua_t *ua, sub_t *s, const sip_sub_state_t *ss)
{
    if (s->st == SUB_ENDING) {
        return; /* the answer to our un-SUBSCRIBE; freed on its response */
    }
    sip_txn_t *t = sip_txn_find(&ua->txl, s->txn);
    if (t) {
        sip_txn_terminate(t);
    }
    s->txn = 0;
    s->dlg_valid = false;
    if (!s->want) {
        sub_free(s);
        return;
    }
    bool lived = ua->now - s->t_started >= SUB_MIN_LIFETIME;
    switch (ss->reason) {
    case SIP_SUBR_NONE:
    case SIP_SUBR_DEACTIVATED:
    case SIP_SUBR_TIMEOUT:
        if (lived && ss->retry_after < 0) {
            /* RFC 6665 §4.1.3: re-subscribe immediately (keep showing the state) */
            LOGI("%s %s: subscription ended, re-subscribing", kind_name(s), s->target);
            s->backoff_ms = 0;
            sub_wait(s, ua->now, 0);
            return;
        }
        sub_fail(ua, s, 0, false, ss->retry_after);
        return;
    case SIP_SUBR_REJECTED:
    case SIP_SUBR_NORESOURCE:
    case SIP_SUBR_INVARIANT:
        sub_fail(ua, s, 0, true, ss->retry_after);
        return;
    case SIP_SUBR_PROBATION:
    case SIP_SUBR_GIVEUP:
    case SIP_SUBR_OTHER:
    default:
        sub_fail(ua, s, 0, false, ss->retry_after);
        return;
    }
}

bool sipua_sub_on_notify(sip_ua_t *ua, sip_txn_t *t, const sip_msg_t *m, const sip_addr_t *from)
{
    sip_str_t pkg = SIP_STR_NULL;
    bool have_event = m->event.p && sip_event_parse(m->event, &pkg, NULL) == 0;

    if (m->to.tag.len == 0) {
        /* out of dialog: only the unsolicited MWI that servers send */
        if (!have_event || !sip_str_ieq_c(pkg, "message-summary")) {
            return false;
        }
        sipua_respond_msg(ua, t, m, from, 200, 0, NULL);
        apply_msgsum(ua, m);
        return true;
    }

    sub_t *s = sub_for_notify(ua, m);
    if (!s) {
        return false;
    }
    const char *want_pkg = s->kind == SUB_KIND_BLF ? "dialog" : "message-summary";
    if (!have_event || !sip_str_ieq_c(pkg, want_pkg)) {
        sipua_respond_msg(ua, t, m, from, 489, 0, "Allow-Events: dialog, message-summary\r\n");
        return true;
    }
    if (!s->dlg_valid) {
        /* NOTIFY before the 2xx: it creates the dialog (we are its UAS) */
        if (sip_dialog_init_uas(&s->dlg, m, s->local_tag) != 0) {
            sipua_respond_msg(ua, t, m, from, 400, 0, NULL);
            return true;
        }
        s->dlg.local_cseq = s->cseq;
        s->dlg.confirmed = true;
        s->dlg_valid = true;
    } else {
        if (s->dlg.remote_cseq_valid && m->cseq <= s->dlg.remote_cseq) {
            sipua_respond_msg(ua, t, m, from, 500, 0, NULL); /* §12.2.2 */
            return true;
        }
        s->dlg.remote_cseq = m->cseq;
        s->dlg.remote_cseq_valid = true;
        if (!s->dlg.remote_tag[0]) {
            sip_str_copy(s->dlg.remote_tag, sizeof(s->dlg.remote_tag), m->from.tag);
        }
        if (m->n_contact > 0) {
            sip_dialog_set_target(&s->dlg, m); /* NOTIFY is a target refresh */
        }
    }
    sipua_respond_msg(ua, t, m, from, 200, 0, NULL);
    if (s->st == SUB_ENDING) {
        return true;
    }

    sip_sub_state_t ss;
    if (!m->sub_state.p || sip_sub_state_parse(m->sub_state, &ss) != 0) {
        ss = (sip_sub_state_t){ .state = SIP_SUBST_ACTIVE, .expires = -1, .retry_after = -1 };
    }
    if (s->kind == SUB_KIND_BLF) {
        apply_dlginfo(ua, s, m);
    } else {
        apply_msgsum(ua, m);
    }
    if (ss.state == SIP_SUBST_TERMINATED) {
        on_terminated(ua, s, &ss);
        return true;
    }
    if (ss.state == SIP_SUBST_ACTIVE) {
        s->backoff_ms = 0;
    }
    if (ss.expires > 0 && s->st == SUB_ACTIVE) {
        /* the notifier may shorten the subscription */
        uint64_t exp = ua->now + (uint64_t)ss.expires * 1000u;
        uint64_t ref = ua->now + (uint64_t)sipua_refresh_delay_s((uint32_t)ss.expires) * 1000u;
        if (exp < s->t_expire) {
            s->t_expire = exp;
        }
        if (ref < s->t_refresh) {
            s->t_refresh = ref;
        }
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* timers                                                               */

void sipua_sub_init(sip_ua_t *ua)
{
    ua->next_sub = 1;
}

void sipua_sub_tick(sip_ua_t *ua)
{
    bool registered = ua->reg.state == SIP_UA_REG_REGISTERED;
    for (int i = 0; i < SIP_UA_MAX_SUBS; i++) {
        sub_t *s = &ua->subs[i];
        switch (s->st) {
        case SUB_WAIT:
            if (registered && ua->now >= s->t_retry && inflight(ua) < SUB_MAX_INFLIGHT) {
                sub_start(ua, s);
            }
            break;
        case SUB_ACTIVE:
            if (s->txn) {
                break;
            }
            if (ua->now >= s->t_expire) {
                LOGW("%s %s: subscription expired, re-subscribing", kind_name(s), s->target);
                sub_wait(s, ua->now, 0);
            } else if ((s->refresh_now || ua->now >= s->t_refresh) && inflight(ua) < SUB_MAX_INFLIGHT) {
                s->refresh_now = false;
                s->t_refresh = s->t_expire; /* until the refresh is answered */
                s->auth_tries = 0;
                if (!sub_send(ua, s, s->expires_req)) {
                    s->t_refresh = ua->now + SUB_NOTXN_RETRY;
                }
            }
            break;
        default:
            break;
        }
    }
}

/* Mirrors sipua_sub_tick: only conditions the tick acts on count. A start or
 * refresh held back by SUB_MAX_INFLIGHT (or by not being registered) is not
 * a deadline: the transaction or registration that ends the hold-back arrives
 * as a packet or its own timer, after which the deadline is asked again. */
uint64_t sipua_sub_next_deadline(const sip_ua_t *ua)
{
    bool registered = ua->reg.state == SIP_UA_REG_REGISTERED;
    bool room = inflight(ua) < SUB_MAX_INFLIGHT;
    uint64_t d = UINT64_MAX;
    for (int i = 0; i < SIP_UA_MAX_SUBS; i++) {
        const sub_t *s = &ua->subs[i];
        uint64_t t = UINT64_MAX;
        switch (s->st) {
        case SUB_WAIT:
            if (registered && room) {
                t = s->t_retry;
            }
            break;
        case SUB_ACTIVE:
            if (s->txn) {
                break;
            }
            t = s->t_expire;
            if (room) {
                uint64_t r = s->refresh_now ? 0 : s->t_refresh;
                if (r < t) {
                    t = r;
                }
            }
            break;
        default:
            break;
        }
        if (t < d) {
            d = t;
        }
    }
    return d;
}

void sipua_sub_registered(sip_ua_t *ua)
{
    for (int i = 0; i < SIP_UA_MAX_SUBS; i++) {
        sub_t *s = &ua->subs[i];
        if (s->st == SUB_WAIT && s->backoff_ms < SUB_BACKOFF_HARD) {
            s->t_retry = 0; /* soft failures (e.g. while unregistered): try again now */
        }
    }
    sipua_sub_tick(ua);
}

void sipua_sub_contact_changed(sip_ua_t *ua)
{
    for (int i = 0; i < SIP_UA_MAX_SUBS; i++) {
        if (ua->subs[i].st == SUB_ACTIVE) {
            ua->subs[i].refresh_now = true;
        }
    }
}

void sipua_sub_terminate_all(sip_ua_t *ua)
{
    for (int i = 0; i < SIP_UA_MAX_SUBS; i++) {
        sub_t *s = &ua->subs[i];
        if (s->st != SUB_FREE && s->want) {
            sip_ua_unsubscribe(ua, s->handle);
        }
    }
}

/* ------------------------------------------------------------------ */
/* public API                                                           */

static sub_t *sub_alloc(sip_ua_t *ua)
{
    sub_t *s = NULL;
    for (int i = 0; i < SIP_UA_MAX_SUBS && !s; i++) {
        if (ua->subs[i].st == SUB_FREE) {
            s = &ua->subs[i];
        }
    }
    for (int i = 0; i < SIP_UA_MAX_SUBS && !s; i++) {
        if (ua->subs[i].st == SUB_ENDING) {
            s = &ua->subs[i]; /* un-SUBSCRIBE already sent: its answer no longer matters */
        }
    }
    if (!s) {
        return NULL;
    }
    sip_txn_t *t = sip_txn_find(&ua->txl, s->txn);
    if (t) {
        sip_txn_terminate(t);
    }
    memset(s, 0, sizeof(*s));
    s->handle = ua->next_sub++;
    if (ua->next_sub <= 0) {
        ua->next_sub = 1;
    }
    s->want = true;
    s->reported = -1;
    s->expires_req = ua->sub_expires;
    s->st = SUB_WAIT;
    return s;
}

static int count_wanted(const sip_ua_t *ua, sub_kind_t kind)
{
    int n = 0;
    for (int i = 0; i < SIP_UA_MAX_SUBS; i++) {
        n += ua->subs[i].st != SUB_FREE && ua->subs[i].want && ua->subs[i].kind == kind;
    }
    return n;
}

sip_err_t sip_ua_subscribe_blf(sip_ua_t *ua, const char *ext, sip_sub_handle_t *out)
{
    if (!ua || !ext || !ext[0] || strlen(ext) >= SIP_UA_EXT_MAX) {
        return SIP_ERR_ARG;
    }
    sipua_enter(ua);
    sip_err_t err = SIP_OK;
    char ruri[SIP_URI_MAX];
    sub_t *s;
    if (sipua_build_ruri(ua, ext, ruri, sizeof(ruri)) != 0) {
        err = SIP_ERR_ARG;
    } else if (count_wanted(ua, SUB_KIND_BLF) >= SIP_UA_MAX_BLF || !(s = sub_alloc(ua))) {
        err = SIP_ERR_NOMEM;
    } else {
        s->kind = SUB_KIND_BLF;
        snprintf(s->target, sizeof(s->target), "%s", ext);
        memcpy(s->ruri, ruri, sizeof(s->ruri));
        report_blf(ua, s, SIP_BLF_UNKNOWN);
        if (out) {
            *out = s->handle;
        }
        sipua_sub_tick(ua);
    }
    sipua_leave(ua);
    return err;
}

sip_err_t sip_ua_subscribe_mwi(sip_ua_t *ua, sip_sub_handle_t *out)
{
    if (!ua) {
        return SIP_ERR_ARG;
    }
    sipua_enter(ua);
    sip_err_t err = SIP_OK;
    sub_t *s;
    if (count_wanted(ua, SUB_KIND_MWI) > 0) {
        err = SIP_ERR_BUSY;
    } else if (!(s = sub_alloc(ua))) {
        err = SIP_ERR_NOMEM;
    } else {
        s->kind = SUB_KIND_MWI;
        snprintf(s->target, sizeof(s->target), "%s", ua->user);
        snprintf(s->ruri, sizeof(s->ruri), "sip:%s@%s", ua->user, ua->hostpart);
        if (out) {
            *out = s->handle;
        }
        sipua_sub_tick(ua);
    }
    sipua_leave(ua);
    return err;
}

sip_err_t sip_ua_unsubscribe(sip_ua_t *ua, sip_sub_handle_t sub)
{
    if (!ua) {
        return SIP_ERR_ARG;
    }
    sipua_enter(ua);
    sip_err_t err = SIP_OK;
    sub_t *s = sub_find(ua, sub);
    if (!s || !s->want) {
        err = SIP_ERR_NOT_FOUND;
    } else {
        s->want = false;
        LOGD("%s %s: unsubscribing", kind_name(s), s->target);
        if (s->st == SUB_ACTIVE) {
            send_unsubscribe(ua, s);
        } else if (s->st != SUB_ENDING) {
            /* WAIT, or TRYING: abandon it; a late NOTIFY gets 481, which ends
             * the subscription at the notifier (RFC 6665 §4.1.3) */
            sip_txn_t *t = sip_txn_find(&ua->txl, s->txn);
            if (t) {
                sip_txn_terminate(t);
            }
            sub_free(s);
        }
    }
    sipua_leave(ua);
    return err;
}

sip_blf_state_t sip_ua_blf_state(const sip_ua_t *ua, sip_sub_handle_t sub)
{
    for (int i = 0; i < SIP_UA_MAX_SUBS; i++) {
        const sub_t *s = &ua->subs[i];
        if (s->st != SUB_FREE && s->handle == sub && s->want && s->kind == SUB_KIND_BLF) {
            return s->reported < 0 ? SIP_BLF_UNKNOWN : (sip_blf_state_t)s->reported;
        }
    }
    return SIP_BLF_UNKNOWN;
}

int sip_ua_debug_sub_slots(const sip_ua_t *ua)
{
    int n = 0;
    for (int i = 0; i < SIP_UA_MAX_SUBS; i++) {
        n += ua->subs[i].st != SUB_FREE;
    }
    return n;
}
