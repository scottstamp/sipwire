/* Subscription scenarios (RFC 6665 subscriber; BLF "dialog" and MWI
 * "message-summary"): fake transport + virtual clock, as test_sip_ua.c. */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "sip_test_util.h"
#include "sip_ua_internal.h"
#include "unity.h"

static fx_t F;
static fx_t *f = &F;

void setUp(void) {}
void tearDown(void)
{
    fx_free(f);
}

#define CHALLENGE "WWW-Authenticate: Digest realm=\"asterisk\",nonce=\"n1\",qop=\"auth\",algorithm=MD5\r\n"
#define CHALLENGE2 "WWW-Authenticate: Digest realm=\"asterisk\",nonce=\"n2\",qop=\"auth\",algorithm=MD5\r\n"

static char sbuf[4096];

static void inject_fmt(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void inject_fmt(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(sbuf, sizeof(sbuf), fmt, ap);
    va_end(ap);
    fx_inject(f, sbuf);
}

static void copy_str(char *dst, size_t cap, sip_str_t s)
{
    TEST_ASSERT_TRUE(s.len < cap);
    memcpy(dst, s.p, s.len);
    dst[s.len] = '\0';
}

/* registered UA (401 → 200), sent log cleared */
static void registered(void (*tweak)(sip_ua_config_t *))
{
    fx_init(f, tweak);
    TEST_ASSERT_EQUAL(SIP_OK, sip_ua_register(f->ua));
    fx_reply(f, 0, 401, "s1", CHALLENGE, NULL);
    fx_reply(f, 1, 200, "s1", "Contact: <sip:1001@10.0.0.2:5060>;expires=300\r\n", NULL);
    TEST_ASSERT_EQUAL(SIP_UA_REG_REGISTERED, sip_ua_reg_state(f->ua));
    fx_clear_sent(f);
    f->auto_reg = true;
}

/* NOTIFY from the notifier in the dialog of the SUBSCRIBE sent[sub_idx]. */
static int s_branch;
static void inject_notify(int sub_idx, const char *from_tag, int cseq, const char *event, const char *substate,
                          const char *ctype, const char *body)
{
    char cid[128], our_tag[80], ruri_user[64];
    const sip_msg_t *m = fx_parse(f, sub_idx);
    copy_str(cid, sizeof(cid), m->call_id);
    copy_str(our_tag, sizeof(our_tag), m->from.tag);
    sip_uri_t u;
    TEST_ASSERT_EQUAL(0, sip_uri_parse(m->ruri, &u));
    copy_str(ruri_user, sizeof(ruri_user), u.user);
    char ct[96] = "";
    if (body) {
        snprintf(ct, sizeof(ct), "Content-Type: %s\r\n", ctype);
    }
    inject_fmt("NOTIFY sip:1001@10.0.0.2:5060 SIP/2.0\r\n"
               "Via: SIP/2.0/UDP 10.0.0.1:5060;rport;branch=z9hG4bKn%d\r\n"
               "Max-Forwards: 70\r\n"
               "From: <sip:%s@10.0.0.1>;tag=%s\r\n"
               "To: \"Alice\" <sip:1001@10.0.0.1>;tag=%s\r\n"
               "Contact: <sip:%s@10.0.0.1:5060>\r\n"
               "Call-ID: %s\r\n"
               "CSeq: %d NOTIFY\r\n"
               "Event: %s\r\n"
               "Subscription-State: %s\r\n"
               "%s"
               "Content-Length: %u\r\n\r\n%s",
               ++s_branch, ruri_user, from_tag, our_tag, ruri_user, cid, cseq, event, substate, ct,
               (unsigned)(body ? strlen(body) : 0), body ? body : "");
}

static char xbuf[1024];
/* Asterisk 20 style full-state dialog-info for extension 1002 */
static const char *dinfo(unsigned version, const char *state, const char *direction)
{
    snprintf(xbuf, sizeof(xbuf),
             "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
             "<dialog-info xmlns=\"urn:ietf:params:xml:ns:dialog-info\" version=\"%u\" state=\"full\" "
             "entity=\"sip:1002@10.0.0.1\">\n"
             " <dialog id=\"1002\"%s%s%s>\n"
             "  <state>%s</state>\n"
             " </dialog>\n"
             "</dialog-info>\n",
             version, direction ? " direction=\"" : "", direction ? direction : "", direction ? "\"" : "", state);
    return xbuf;
}

#define DLG_CT "application/dialog-info+xml"
#define MWI_CT "application/simple-message-summary"

static void notify_blf(int sub_idx, int cseq, unsigned version, const char *state, const char *dir)
{
    inject_notify(sub_idx, "srv1", cseq, "dialog", "active;expires=600", DLG_CT, dinfo(version, state, dir));
}

static int last_status(void)
{
    int r = fx_find(f, "SIP/2.0 ", 0);
    TEST_ASSERT_TRUE(r >= 0);
    return fx_parse(f, r)->status;
}

static const fx_ev_t *last_blf(void)
{
    const fx_ev_t *e = fx_last_ev(f, FXE_BLF);
    TEST_ASSERT_NOT_NULL(e);
    return e;
}

/* subscribe to ext, answer the SUBSCRIBE with 200 (to-tag srv1); returns its index */
static int blf_established(const char *ext, sip_sub_handle_t *h)
{
    TEST_ASSERT_EQUAL(SIP_OK, sip_ua_subscribe_blf(f->ua, ext, h));
    int i = fx_find(f, "SUBSCRIBE", 0);
    TEST_ASSERT_TRUE(i >= 0);
    fx_reply(f, i, 200, "srv1", "Expires: 600\r\nContact: <sip:srv@10.0.0.1:5060>\r\n", NULL);
    return i;
}

/* ------------------------------------------------------------------ */

static void test_blf_subscribe_auth_notify_states(void)
{
    registered(NULL);
    sip_sub_handle_t h = 0;
    TEST_ASSERT_EQUAL(SIP_OK, sip_ua_subscribe_blf(f->ua, "1002", &h));
    TEST_ASSERT_TRUE(h > 0);
    TEST_ASSERT_EQUAL(SIP_BLF_UNKNOWN, last_blf()->state);
    TEST_ASSERT_EQUAL_STRING("1002", last_blf()->number);
    TEST_ASSERT_EQUAL(1, fx_count(f, "SUBSCRIBE"));
    const sip_msg_t *m = fx_parse(f, 0);
    TEST_ASSERT_EQUAL(SIP_METHOD_SUBSCRIBE, m->method);
    TEST_ASSERT_TRUE(sip_str_eq(m->ruri, sip_str_c("sip:1002@10.0.0.1")));
    TEST_ASSERT_EQUAL(600, m->expires);
    TEST_ASSERT_EQUAL_STRING("dialog", fx_hdr(f, 0, "Event"));
    TEST_ASSERT_EQUAL_STRING("application/dialog-info+xml", fx_hdr(f, 0, "Accept"));
    TEST_ASSERT_EQUAL_STRING("<sip:1002@10.0.0.1>", fx_hdr(f, 0, "To"));
    TEST_ASSERT_EQUAL_STRING("<sip:1001@10.0.0.2:5060>", fx_hdr(f, 0, "Contact"));
    TEST_ASSERT_NOT_NULL(strstr(fx_hdr(f, 0, "Authorization"), "uri=\"sip:1002@10.0.0.1\""));

    /* challenge with a new nonce: retried once, same Call-ID, next CSeq */
    char cid[128];
    copy_str(cid, sizeof(cid), m->call_id);
    fx_reply(f, 0, 401, "x", CHALLENGE2, NULL);
    TEST_ASSERT_EQUAL(2, fx_count(f, "SUBSCRIBE"));
    m = fx_parse(f, 1);
    TEST_ASSERT_TRUE(sip_str_eq(m->call_id, sip_str_c(cid)));
    TEST_ASSERT_EQUAL_UINT32(2, m->cseq);
    TEST_ASSERT_NOT_NULL(strstr(fx_hdr(f, 1, "Authorization"), "nonce=\"n2\""));
    fx_reply(f, 1, 200, "srv1", "Expires: 600\r\nContact: <sip:srv@10.0.0.1:5060>\r\n", NULL);

    notify_blf(1, 1, 0, "terminated", NULL);
    TEST_ASSERT_EQUAL(200, last_status());
    TEST_ASSERT_EQUAL(SIP_BLF_IDLE, last_blf()->state);
    TEST_ASSERT_EQUAL(h, last_blf()->call);
    notify_blf(1, 2, 1, "early", "recipient");
    TEST_ASSERT_EQUAL(SIP_BLF_RINGING, last_blf()->state);
    TEST_ASSERT_EQUAL(SIP_BLF_RINGING, sip_ua_blf_state(f->ua, h));
    notify_blf(1, 3, 2, "confirmed", "recipient");
    TEST_ASSERT_EQUAL(SIP_BLF_BUSY, last_blf()->state);
    int nev = fx_count_ev(f, FXE_BLF);
    notify_blf(1, 4, 2, "terminated", NULL); /* same version: stale, ignored */
    TEST_ASSERT_EQUAL(200, last_status());
    TEST_ASSERT_EQUAL(nev, fx_count_ev(f, FXE_BLF));
    notify_blf(1, 5, 3, "terminated", NULL);
    TEST_ASSERT_EQUAL(SIP_BLF_IDLE, last_blf()->state);
    /* lower CSeq in the dialog: 500 */
    notify_blf(1, 2, 9, "confirmed", NULL);
    TEST_ASSERT_EQUAL(500, last_status());
    TEST_ASSERT_EQUAL(SIP_BLF_IDLE, last_blf()->state);
    /* placing a call (initiator, early) is busy, not ringing */
    notify_blf(1, 6, 4, "early", "initiator");
    TEST_ASSERT_EQUAL(SIP_BLF_BUSY, last_blf()->state);
    /* wrong event package in the dialog: 489 */
    inject_notify(1, "srv1", 7, "presence", "active", DLG_CT, dinfo(5, "terminated", NULL));
    TEST_ASSERT_EQUAL(489, last_status());
}

static void test_blf_waits_for_registration(void)
{
    fx_init(f, NULL);
    sip_sub_handle_t h;
    TEST_ASSERT_EQUAL(SIP_OK, sip_ua_subscribe_blf(f->ua, "1002", &h));
    fx_advance(f, 5000);
    TEST_ASSERT_EQUAL(0, fx_count(f, "SUBSCRIBE"));
    sip_ua_register(f->ua);
    fx_reply(f, 0, 401, "s1", CHALLENGE, NULL);
    TEST_ASSERT_EQUAL(0, fx_count(f, "SUBSCRIBE"));
    fx_reply(f, 1, 200, "s1", "Expires: 300\r\n", NULL);
    TEST_ASSERT_EQUAL(1, fx_count(f, "SUBSCRIBE"));
}

static void test_notify_before_200(void)
{
    registered(NULL);
    sip_sub_handle_t h;
    TEST_ASSERT_EQUAL(SIP_OK, sip_ua_subscribe_blf(f->ua, "1003", &h));
    int s = fx_find(f, "SUBSCRIBE", 0);
    inject_notify(s, "srvA", 1, "dialog", "active;expires=600", DLG_CT, dinfo(0, "confirmed", NULL));
    TEST_ASSERT_EQUAL(200, last_status());
    TEST_ASSERT_EQUAL(SIP_BLF_BUSY, last_blf()->state);
    fx_reply(f, s, 200, "srvA", "Expires: 600\r\n", NULL);
    /* the refresh is in the dialog the NOTIFY created (remote tag srvA) */
    fx_advance(f, 300000 + 20);
    int r = fx_find(f, "SUBSCRIBE", 0);
    TEST_ASSERT_TRUE(r > s);
    const sip_msg_t *m = fx_parse(f, r);
    TEST_ASSERT_TRUE(sip_str_eq(m->to.tag, sip_str_c("srvA")));
    TEST_ASSERT_TRUE(sip_str_eq(m->ruri, sip_str_c("sip:1003@10.0.0.1:5060"))); /* NOTIFY Contact */
    TEST_ASSERT_EQUAL_UINT32(2, m->cseq);
}

static void test_refresh_timing(void)
{
    registered(NULL);
    sip_sub_handle_t h;
    TEST_ASSERT_EQUAL(SIP_OK, sip_ua_subscribe_blf(f->ua, "1002", &h));
    int s = fx_find(f, "SUBSCRIBE", 0);
    /* server grants 120 s: refresh at min(60, 90) = 60 s */
    fx_reply(f, s, 200, "srv1", "Expires: 120\r\nContact: <sip:srv@10.0.0.1:5060>\r\n", NULL);
    fx_advance(f, 59990);
    TEST_ASSERT_EQUAL(1, fx_count(f, "SUBSCRIBE"));
    fx_advance(f, 20);
    TEST_ASSERT_EQUAL(2, fx_count(f, "SUBSCRIBE"));
    int r = fx_find(f, "SUBSCRIBE", 0);
    const sip_msg_t *m = fx_parse(f, r);
    TEST_ASSERT_TRUE(sip_str_eq(m->to.tag, sip_str_c("srv1")));
    TEST_ASSERT_TRUE(sip_str_eq(m->ruri, sip_str_c("sip:srv@10.0.0.1:5060")));
    TEST_ASSERT_EQUAL(600, m->expires);
    TEST_ASSERT_NOT_NULL(fx_hdr(f, r, "Authorization"));
    /* 600 granted now: next refresh after 300 s (min(300, 570)) */
    fx_reply(f, r, 200, NULL, "Expires: 600\r\n", NULL);
    fx_advance(f, 299990);
    TEST_ASSERT_EQUAL(2, fx_count(f, "SUBSCRIBE"));
    fx_advance(f, 20);
    TEST_ASSERT_EQUAL(3, fx_count(f, "SUBSCRIBE"));
    /* the notifier shortens it with Subscription-State expires=40: refresh at 10 s */
    r = fx_find(f, "SUBSCRIBE", 0);
    fx_reply(f, r, 200, NULL, "Expires: 600\r\n", NULL);
    notify_blf(s, 1, 0, "terminated", NULL);
    inject_notify(s, "srv1", 2, "dialog", "active;expires=40", DLG_CT, dinfo(1, "terminated", NULL));
    fx_advance(f, 9990);
    TEST_ASSERT_EQUAL(3, fx_count(f, "SUBSCRIBE"));
    fx_advance(f, 20);
    TEST_ASSERT_EQUAL(4, fx_count(f, "SUBSCRIBE"));
    /* refresh times out (timer F): the subscription is retried from scratch */
    fx_advance(f, 33000);
    TEST_ASSERT_EQUAL(SIP_BLF_UNKNOWN, last_blf()->state);
}

static void test_terminated_noresource_backoff(void)
{
    registered(NULL);
    sip_sub_handle_t h;
    int s = blf_established("1002", &h);
    notify_blf(s, 1, 0, "terminated", NULL);
    TEST_ASSERT_EQUAL(SIP_BLF_IDLE, last_blf()->state);
    char cid[128];
    copy_str(cid, sizeof(cid), fx_parse(f, s)->call_id);
    inject_notify(s, "srv1", 2, "dialog", "terminated;reason=noresource", NULL, NULL);
    TEST_ASSERT_EQUAL(200, last_status());
    TEST_ASSERT_EQUAL(SIP_BLF_UNKNOWN, last_blf()->state);
    /* don't hammer: first retry after 60 s */
    fx_advance(f, 59990);
    TEST_ASSERT_EQUAL(1, fx_count(f, "SUBSCRIBE"));
    fx_advance(f, 20);
    TEST_ASSERT_EQUAL(2, fx_count(f, "SUBSCRIBE"));
    int r = fx_find(f, "SUBSCRIBE", 0);
    const sip_msg_t *m = fx_parse(f, r);
    TEST_ASSERT_FALSE(sip_str_eq(m->call_id, sip_str_c(cid))); /* a new subscription */
    TEST_ASSERT_EQUAL(0, m->to.tag.len);
    /* 404 again and again: 120 s, 240 s, then capped at 300 s */
    fx_reply(f, r, 404, "x", NULL, NULL);
    fx_advance(f, 119990);
    TEST_ASSERT_EQUAL(2, fx_count(f, "SUBSCRIBE"));
    fx_advance(f, 20);
    TEST_ASSERT_EQUAL(3, fx_count(f, "SUBSCRIBE"));
    fx_reply(f, fx_find(f, "SUBSCRIBE", 0), 404, "x", NULL, NULL);
    fx_advance(f, 240010);
    TEST_ASSERT_EQUAL(4, fx_count(f, "SUBSCRIBE"));
    fx_reply(f, fx_find(f, "SUBSCRIBE", 0), 404, "x", NULL, NULL);
    fx_advance(f, 299990);
    TEST_ASSERT_EQUAL(4, fx_count(f, "SUBSCRIBE"));
    fx_advance(f, 20);
    TEST_ASSERT_EQUAL(5, fx_count(f, "SUBSCRIBE"));
    /* success resets the backoff */
    r = fx_find(f, "SUBSCRIBE", 0);
    fx_reply(f, r, 200, "srv2", "Expires: 600\r\n", NULL);
    inject_notify(r, "srv2", 1, "dialog", "active;expires=600", DLG_CT, dinfo(0, "confirmed", NULL));
    TEST_ASSERT_EQUAL(SIP_BLF_BUSY, last_blf()->state);
}

static void test_terminated_retry_after_and_deactivated(void)
{
    registered(NULL);
    sip_sub_handle_t h;
    int s = blf_established("1002", &h);
    notify_blf(s, 1, 0, "confirmed", NULL);
    /* probation with retry-after=30 */
    inject_notify(s, "srv1", 2, "dialog", "terminated;reason=probation;retry-after=30", NULL, NULL);
    TEST_ASSERT_EQUAL(SIP_BLF_UNKNOWN, last_blf()->state);
    fx_advance(f, 29990);
    TEST_ASSERT_EQUAL(1, fx_count(f, "SUBSCRIBE"));
    fx_advance(f, 20);
    TEST_ASSERT_EQUAL(2, fx_count(f, "SUBSCRIBE"));
    int r = fx_find(f, "SUBSCRIBE", 0);
    fx_reply(f, r, 200, "srv3", "Expires: 600\r\n", NULL);
    inject_notify(r, "srv3", 1, "dialog", "active;expires=600", DLG_CT, dinfo(0, "confirmed", NULL));
    TEST_ASSERT_EQUAL(SIP_BLF_BUSY, last_blf()->state);
    /* deactivated after a while: re-subscribe at once and keep the lamp */
    fx_advance(f, 40000);
    int nev = fx_count_ev(f, FXE_BLF);
    inject_notify(r, "srv3", 2, "dialog", "terminated;reason=deactivated", NULL, NULL);
    TEST_ASSERT_EQUAL(nev, fx_count_ev(f, FXE_BLF));
    fx_advance(f, 20);
    TEST_ASSERT_EQUAL(3, fx_count(f, "SUBSCRIBE"));
    /* ... but a notifier that ends it right away again gets backoff, not a loop */
    r = fx_find(f, "SUBSCRIBE", 0);
    fx_reply(f, r, 200, "srv4", "Expires: 600\r\n", NULL);
    inject_notify(r, "srv4", 1, "dialog", "terminated;reason=deactivated", NULL, NULL);
    fx_advance(f, 4990);
    TEST_ASSERT_EQUAL(3, fx_count(f, "SUBSCRIBE"));
    fx_advance(f, 20);
    TEST_ASSERT_EQUAL(4, fx_count(f, "SUBSCRIBE"));
    /* 503 with Retry-After: 20 */
    fx_reply(f, fx_find(f, "SUBSCRIBE", 0), 503, "x", "Retry-After: 20\r\n", NULL);
    fx_advance(f, 19990);
    TEST_ASSERT_EQUAL(4, fx_count(f, "SUBSCRIBE"));
    fx_advance(f, 20);
    TEST_ASSERT_EQUAL(5, fx_count(f, "SUBSCRIBE"));
}

static void test_refresh_481_resubscribes(void)
{
    registered(NULL);
    sip_sub_handle_t h;
    int s = blf_established("1002", &h);
    notify_blf(s, 1, 0, "terminated", NULL);
    fx_advance(f, 300010);
    int r = fx_find(f, "SUBSCRIBE", 0);
    TEST_ASSERT_TRUE(fx_parse(f, r)->to.tag.len > 0);
    fx_reply(f, r, 481, NULL, NULL, NULL);
    fx_advance(f, 20);
    int n = fx_find(f, "SUBSCRIBE", 0);
    TEST_ASSERT_TRUE(n > r);
    TEST_ASSERT_EQUAL(0, fx_parse(f, n)->to.tag.len);
    TEST_ASSERT_EQUAL(SIP_BLF_IDLE, last_blf()->state); /* kept while re-subscribing */
}

static void test_subscribe_423(void)
{
    registered(NULL);
    sip_sub_handle_t h;
    TEST_ASSERT_EQUAL(SIP_OK, sip_ua_subscribe_blf(f->ua, "1002", &h));
    fx_reply(f, 0, 423, "x", "Min-Expires: 1800\r\n", NULL);
    TEST_ASSERT_EQUAL(2, fx_count(f, "SUBSCRIBE"));
    TEST_ASSERT_EQUAL(1800, fx_parse(f, 1)->expires);
}

static void test_unknown_notify_481(void)
{
    registered(NULL);
    inject_fmt("NOTIFY sip:1001@10.0.0.2:5060 SIP/2.0\r\n"
               "Via: SIP/2.0/UDP 10.0.0.1:5060;branch=z9hG4bKu1\r\n"
               "From: <sip:1002@10.0.0.1>;tag=a\r\nTo: <sip:1001@10.0.0.1>;tag=nosuch\r\n"
               "Call-ID: unknown@x\r\nCSeq: 1 NOTIFY\r\nEvent: dialog\r\n"
               "Subscription-State: active\r\nContent-Length: 0\r\n\r\n");
    TEST_ASSERT_EQUAL(481, last_status());
    /* out of dialog, not message-summary */
    inject_fmt("NOTIFY sip:1001@10.0.0.2:5060 SIP/2.0\r\n"
               "Via: SIP/2.0/UDP 10.0.0.1:5060;branch=z9hG4bKu2\r\n"
               "From: <sip:1002@10.0.0.1>;tag=a\r\nTo: <sip:1001@10.0.0.1>\r\n"
               "Call-ID: unknown2@x\r\nCSeq: 1 NOTIFY\r\nEvent: check-sync\r\n"
               "Content-Length: 0\r\n\r\n");
    TEST_ASSERT_EQUAL(481, last_status());
    TEST_ASSERT_EQUAL(0, fx_count_ev(f, FXE_BLF));
    TEST_ASSERT_EQUAL(0, fx_count_ev(f, FXE_MWI));
}

static void unsolicited_mwi(int n, const char *body)
{
    inject_fmt("NOTIFY sip:1001@10.0.0.2:5060 SIP/2.0\r\n"
               "Via: SIP/2.0/UDP 10.0.0.1:5060;rport;branch=z9hG4bKmwi%d\r\n"
               "From: <sip:1001@10.0.0.1>;tag=m%d\r\nTo: <sip:1001@10.0.0.2:5060>\r\n"
               "Contact: <sip:10.0.0.1:5060>\r\n"
               "Call-ID: mwi%d@10.0.0.1\r\nCSeq: 1 NOTIFY\r\nEvent: message-summary\r\n"
               "Subscription-State: terminated\r\n"
               "Content-Type: application/simple-message-summary\r\n"
               "Content-Length: %u\r\n\r\n%s",
               n, n, n, (unsigned)strlen(body), body);
}

static void test_unsolicited_mwi(void)
{
    registered(NULL);
    unsolicited_mwi(1, "Messages-Waiting: yes\r\nMessage-Account: sip:1001@10.0.0.1\r\n"
                       "Voice-Message: 2/8 (1/0)\r\n");
    TEST_ASSERT_EQUAL(200, last_status());
    const fx_ev_t *e = fx_last_ev(f, FXE_MWI);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL(1, e->state);
    TEST_ASSERT_EQUAL(2, e->reason);
    TEST_ASSERT_EQUAL(8, e->code);
    /* the same again: no new report */
    unsolicited_mwi(2, "Messages-Waiting: yes\r\nVoice-Message: 2/8 (1/0)\r\n");
    TEST_ASSERT_EQUAL(1, fx_count_ev(f, FXE_MWI));
    unsolicited_mwi(3, "Messages-Waiting: no\r\nVoice-Message: 0/10\r\n");
    TEST_ASSERT_EQUAL(2, fx_count_ev(f, FXE_MWI));
    e = fx_last_ev(f, FXE_MWI);
    TEST_ASSERT_EQUAL(0, e->state);
    TEST_ASSERT_EQUAL(0, e->reason);
    TEST_ASSERT_EQUAL(10, e->code);
    /* malformed body: answered, ignored */
    unsolicited_mwi(4, "Voice-Message: 3/3\r\n");
    TEST_ASSERT_EQUAL(200, last_status());
    TEST_ASSERT_EQUAL(2, fx_count_ev(f, FXE_MWI));
}

static void test_mwi_subscription(void)
{
    registered(NULL);
    sip_sub_handle_t h = 0;
    TEST_ASSERT_EQUAL(SIP_OK, sip_ua_subscribe_mwi(f->ua, &h));
    TEST_ASSERT_TRUE(h > 0);
    TEST_ASSERT_EQUAL(SIP_ERR_BUSY, sip_ua_subscribe_mwi(f->ua, NULL));
    int s = fx_find(f, "SUBSCRIBE", 0);
    TEST_ASSERT_TRUE(s >= 0);
    const sip_msg_t *m = fx_parse(f, s);
    TEST_ASSERT_TRUE(sip_str_eq(m->ruri, sip_str_c("sip:1001@10.0.0.1")));
    TEST_ASSERT_EQUAL_STRING("message-summary", fx_hdr(f, s, "Event"));
    TEST_ASSERT_EQUAL_STRING("application/simple-message-summary", fx_hdr(f, s, "Accept"));
    fx_reply(f, s, 200, "mw", "Expires: 600\r\n", NULL);
    inject_notify(s, "mw", 1, "message-summary", "active;expires=600", MWI_CT,
                  "Messages-Waiting: yes\r\nMessage-Account: sip:1001@10.0.0.1\r\nVoice-Message: 1/0\r\n");
    TEST_ASSERT_EQUAL(200, last_status());
    const fx_ev_t *e = fx_last_ev(f, FXE_MWI);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL(1, e->state);
    TEST_ASSERT_EQUAL(1, e->reason);
    TEST_ASSERT_EQUAL(0, fx_count_ev(f, FXE_BLF)); /* no BLF reports for MWI */
    /* a dialog NOTIFY in the MWI dialog is the wrong package */
    inject_notify(s, "mw", 2, "dialog", "active", DLG_CT, dinfo(0, "confirmed", NULL));
    TEST_ASSERT_EQUAL(489, last_status());
    TEST_ASSERT_EQUAL(SIP_OK, sip_ua_unsubscribe(f->ua, h));
    TEST_ASSERT_EQUAL(SIP_OK, sip_ua_subscribe_mwi(f->ua, NULL));
}

static void test_unsubscribe(void)
{
    registered(NULL);
    sip_sub_handle_t h;
    int s = blf_established("1002", &h);
    notify_blf(s, 1, 0, "confirmed", NULL);
    int nev = fx_count_ev(f, FXE_BLF);
    TEST_ASSERT_EQUAL(SIP_OK, sip_ua_unsubscribe(f->ua, h));
    TEST_ASSERT_EQUAL(SIP_ERR_NOT_FOUND, sip_ua_unsubscribe(f->ua, h));
    int u = fx_find(f, "SUBSCRIBE", 0);
    const sip_msg_t *m = fx_parse(f, u);
    TEST_ASSERT_EQUAL(0, m->expires);
    TEST_ASSERT_TRUE(sip_str_eq(m->to.tag, sip_str_c("srv1")));
    /* the final NOTIFY is answered, but reports nothing */
    inject_notify(s, "srv1", 2, "dialog", "terminated;reason=timeout", DLG_CT, dinfo(1, "terminated", NULL));
    TEST_ASSERT_EQUAL(200, last_status());
    TEST_ASSERT_EQUAL(nev, fx_count_ev(f, FXE_BLF));
    TEST_ASSERT_EQUAL(1, sip_ua_debug_sub_slots(f->ua));
    fx_reply(f, u, 200, NULL, "Expires: 0\r\n", NULL);
    TEST_ASSERT_EQUAL(0, sip_ua_debug_sub_slots(f->ua));
    /* afterwards the dialog is unknown */
    notify_blf(s, 3, 2, "confirmed", NULL);
    TEST_ASSERT_EQUAL(481, last_status());

    /* unsubscribe while the SUBSCRIBE is in flight: slot freed, no un-SUBSCRIBE */
    TEST_ASSERT_EQUAL(SIP_OK, sip_ua_subscribe_blf(f->ua, "1003", &h));
    int t = fx_find(f, "SUBSCRIBE", 0);
    TEST_ASSERT_EQUAL(SIP_OK, sip_ua_unsubscribe(f->ua, h));
    TEST_ASSERT_EQUAL(0, sip_ua_debug_sub_slots(f->ua));
    int n = fx_count(f, "SUBSCRIBE");
    fx_reply(f, t, 200, "srv9", "Expires: 600\r\n", NULL);
    TEST_ASSERT_EQUAL(n, fx_count(f, "SUBSCRIBE"));
    fx_advance(f, 64000);
    TEST_ASSERT_EQUAL(0, sip_ua_debug_txn_count(f->ua));
}

static void test_unregister_ends_subscriptions(void)
{
    registered(NULL);
    sip_sub_handle_t h1, h2;
    int s1 = blf_established("1002", &h1);
    int s2 = blf_established("1003", &h2);
    (void)s1;
    (void)s2;
    TEST_ASSERT_EQUAL(SIP_OK, sip_ua_subscribe_mwi(f->ua, NULL)); /* in flight */
    fx_clear_sent(f);
    TEST_ASSERT_EQUAL(SIP_OK, sip_ua_unregister(f->ua));
    TEST_ASSERT_EQUAL(2, fx_count(f, "SUBSCRIBE"));
    for (int i = 0; i < f->n_sent; i++) {
        if (strncmp(f->sent[i].buf, "SUBSCRIBE", 9) == 0) {
            TEST_ASSERT_EQUAL(0, fx_parse(f, i)->expires);
        }
    }
    TEST_ASSERT_EQUAL(1, fx_count(f, "REGISTER"));
    TEST_ASSERT_EQUAL(SIP_BLF_UNKNOWN, sip_ua_blf_state(f->ua, h1));
    TEST_ASSERT_EQUAL(SIP_ERR_NOT_FOUND, sip_ua_unsubscribe(f->ua, h2));
}

/* 8 BLF + MWI right after registration, NOTIFYs racing the 2xx, while a call
 * is ringing: the transaction pool (with eviction) must cope, nothing gets a
 * 503, everything ends up established. */
static void test_burst_of_nine(void)
{
    registered(NULL);
    sip_call_handle_t call;
    TEST_ASSERT_EQUAL(SIP_OK, sip_call_dial(f->ua, "600", &call));
    int inv = fx_find(f, "INVITE", 0);
    fx_reply(f, inv, 180, "c1", NULL, NULL);
    sip_sub_handle_t h[9];
    char ext[8];
    for (int i = 0; i < 8; i++) {
        snprintf(ext, sizeof(ext), "10%02d", i + 10);
        TEST_ASSERT_EQUAL(SIP_OK, sip_ua_subscribe_blf(f->ua, ext, &h[i]));
    }
    TEST_ASSERT_EQUAL(SIP_ERR_NOMEM, sip_ua_subscribe_blf(f->ua, "1099", NULL));
    TEST_ASSERT_EQUAL(SIP_OK, sip_ua_subscribe_mwi(f->ua, &h[8]));
    /* paced: three SUBSCRIBEs in flight at a time */
    TEST_ASSERT_EQUAL(3, fx_count(f, "SUBSCRIBE"));
    int answered = 0;
    for (int round = 0; round < 20 && answered < 9; round++) {
        int n = f->n_sent;
        for (int i = 0; i < n; i++) {
            if (strncmp(f->sent[i].buf, "SUBSCRIBE", 9) != 0 || f->sent[i].to.port == 0) {
                continue;
            }
            f->sent[i].to.port = 0; /* mark handled */
            char tag[16];
            snprintf(tag, sizeof(tag), "n%d", i);
            /* NOTIFY first, then the 200 (and a retransmitted NOTIFY) */
            bool mwi = strstr(f->sent[i].buf, "Event: message-summary") != NULL;
            if (mwi) {
                inject_notify(i, tag, 1, "message-summary", "active;expires=600", MWI_CT,
                              "Messages-Waiting: yes\r\nVoice-Message: 3/0\r\n");
            } else {
                inject_notify(i, tag, 1, "dialog", "active;expires=600", DLG_CT, dinfo(0, "terminated", NULL));
            }
            TEST_ASSERT_EQUAL(200, last_status());
            fx_reply(f, i, 200, tag, "Expires: 600\r\n", NULL);
            answered++;
        }
        fx_advance(f, 20);
    }
    TEST_ASSERT_EQUAL(9, answered);
    TEST_ASSERT_EQUAL(9, fx_count(f, "SUBSCRIBE"));
    TEST_ASSERT_EQUAL(0, fx_count(f, "SIP/2.0 503"));
    for (int i = 0; i < 8; i++) {
        TEST_ASSERT_EQUAL(SIP_BLF_IDLE, sip_ua_blf_state(f->ua, h[i]));
    }
    TEST_ASSERT_EQUAL(1, fx_count_ev(f, FXE_MWI));
    TEST_ASSERT_TRUE(sip_ua_debug_txn_count(f->ua) <= 16);
    /* the call still works */
    fx_reply(f, inv, 200, "c1", NULL, FX_SDP_G722);
    TEST_ASSERT_EQUAL(SIP_UA_CALL_ACTIVE, sip_call_state(f->ua, call));
    /* all nine refresh at the same time: paced as well, all succeed */
    fx_clear_sent(f);
    fx_advance(f, 300010);
    int refreshed = 0;
    for (int round = 0; round < 20 && refreshed < 9; round++) {
        int n = f->n_sent;
        for (int i = 0; i < n; i++) {
            if (strncmp(f->sent[i].buf, "SUBSCRIBE", 9) == 0 && f->sent[i].to.port != 0) {
                f->sent[i].to.port = 0;
                fx_reply(f, i, 200, NULL, "Expires: 600\r\n", NULL);
                refreshed++;
            }
        }
        fx_advance(f, 20);
    }
    TEST_ASSERT_EQUAL(9, refreshed);
    for (int i = 0; i < 8; i++) {
        TEST_ASSERT_EQUAL(SIP_BLF_IDLE, sip_ua_blf_state(f->ua, h[i]));
    }
}

static void test_blf_arg_checks(void)
{
    registered(NULL);
    sip_sub_handle_t h;
    TEST_ASSERT_EQUAL(SIP_ERR_ARG, sip_ua_subscribe_blf(f->ua, "", &h));
    TEST_ASSERT_EQUAL(SIP_ERR_ARG, sip_ua_subscribe_blf(f->ua, "0123456789012345678901234567890123", &h));
    TEST_ASSERT_EQUAL(SIP_ERR_ARG, sip_ua_subscribe_blf(f->ua, "a\x01", &h));
    TEST_ASSERT_EQUAL(SIP_ERR_NOT_FOUND, sip_ua_unsubscribe(f->ua, 12345));
    TEST_ASSERT_EQUAL(SIP_BLF_UNKNOWN, sip_ua_blf_state(f->ua, 12345));
    /* "*" and "#" are escaped in the URI */
    TEST_ASSERT_EQUAL(SIP_OK, sip_ua_subscribe_blf(f->ua, "*8#1", &h));
    TEST_ASSERT_TRUE(sip_str_eq(fx_parse(f, fx_find(f, "SUBSCRIBE", 0))->ruri, sip_str_c("sip:*8%231@10.0.0.1")));
}

/* partial state with a gap in versions → refresh to get full state */
static void test_blf_partial_versions(void)
{
    registered(NULL);
    sip_sub_handle_t h;
    int s = blf_established("1002", &h);
    notify_blf(s, 1, 5, "terminated", NULL);
    TEST_ASSERT_EQUAL(SIP_BLF_IDLE, last_blf()->state);
    const char *partial =
        "<dialog-info xmlns=\"urn:ietf:params:xml:ns:dialog-info\" version=\"6\" state=\"partial\" "
        "entity=\"sip:1002@10.0.0.1\"><dialog id=\"a\" direction=\"recipient\"><state>early</state></dialog>"
        "</dialog-info>";
    inject_notify(s, "srv1", 2, "dialog", "active", DLG_CT, partial);
    TEST_ASSERT_EQUAL(SIP_BLF_RINGING, last_blf()->state);
    const char *partial2 =
        "<dialog-info xmlns=\"urn:ietf:params:xml:ns:dialog-info\" version=\"7\" state=\"partial\" "
        "entity=\"sip:1002@10.0.0.1\"><dialog id=\"b\"><state>confirmed</state></dialog>"
        "</dialog-info>";
    inject_notify(s, "srv1", 3, "dialog", "active", DLG_CT, partial2);
    TEST_ASSERT_EQUAL(SIP_BLF_RINGING, last_blf()->state); /* a still rings, b is busy */
    const char *partial3 =
        "<dialog-info xmlns=\"urn:ietf:params:xml:ns:dialog-info\" version=\"8\" state=\"partial\" "
        "entity=\"sip:1002@10.0.0.1\"><dialog id=\"a\"><state>terminated</state></dialog>"
        "</dialog-info>";
    inject_notify(s, "srv1", 4, "dialog", "active", DLG_CT, partial3);
    TEST_ASSERT_EQUAL(SIP_BLF_BUSY, last_blf()->state);
    TEST_ASSERT_EQUAL(1, fx_count(f, "SUBSCRIBE"));
    const char *gap =
        "<dialog-info xmlns=\"urn:ietf:params:xml:ns:dialog-info\" version=\"11\" state=\"partial\" "
        "entity=\"sip:1002@10.0.0.1\"><dialog id=\"b\"><state>terminated</state></dialog>"
        "</dialog-info>";
    inject_notify(s, "srv1", 5, "dialog", "active", DLG_CT, gap);
    TEST_ASSERT_EQUAL(SIP_BLF_IDLE, last_blf()->state);
    fx_advance(f, 20);
    TEST_ASSERT_EQUAL(2, fx_count(f, "SUBSCRIBE")); /* refresh for full state */
}

static void unsubscribe_cb_tweak(sip_ua_config_t *cfg)
{
    cfg->sub_expires = 3600;
}

static void test_sub_expires_config(void)
{
    registered(unsubscribe_cb_tweak);
    sip_sub_handle_t h;
    TEST_ASSERT_EQUAL(SIP_OK, sip_ua_subscribe_blf(f->ua, "1002", &h));
    TEST_ASSERT_EQUAL(3600, fx_parse(f, 0)->expires);
    /* no answer at all: timer F, UNKNOWN, retried after 5 s */
    fx_advance(f, 32010);
    TEST_ASSERT_EQUAL(SIP_BLF_UNKNOWN, last_blf()->state);
    int n = fx_count(f, "SUBSCRIBE");
    fx_advance(f, 5010);
    TEST_ASSERT_EQUAL(n + 1, fx_count(f, "SUBSCRIBE"));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_blf_subscribe_auth_notify_states);
    RUN_TEST(test_blf_waits_for_registration);
    RUN_TEST(test_notify_before_200);
    RUN_TEST(test_refresh_timing);
    RUN_TEST(test_terminated_noresource_backoff);
    RUN_TEST(test_terminated_retry_after_and_deactivated);
    RUN_TEST(test_refresh_481_resubscribes);
    RUN_TEST(test_subscribe_423);
    RUN_TEST(test_unknown_notify_481);
    RUN_TEST(test_unsolicited_mwi);
    RUN_TEST(test_mwi_subscription);
    RUN_TEST(test_unsubscribe);
    RUN_TEST(test_unregister_ends_subscriptions);
    RUN_TEST(test_burst_of_nine);
    RUN_TEST(test_blf_arg_checks);
    RUN_TEST(test_blf_partial_versions);
    RUN_TEST(test_sub_expires_config);
    return UNITY_END();
}
