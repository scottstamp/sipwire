/* UA scenario tests: fake transport + virtual clock drive registration,
 * transactions (timers A/B/E/F/G/H/K...), dialogs and calls. */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "sip_auth.h"
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

/* ---------------------------------------------------------------- helpers */

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

/* incoming INVITE from 1002 "Bob" via the server */
static void inject_invite(const char *callid, const char *branch, int cseq, const char *sdp, const char *extra)
{
    inject_fmt("INVITE sip:1001@10.0.0.2:5060 SIP/2.0\r\n"
               "Via: SIP/2.0/UDP 10.0.0.1:5060;rport;branch=%s\r\n"
               "Max-Forwards: 70\r\n"
               "From: \"Bob\" <sip:1002@10.0.0.1>;tag=remote1\r\n"
               "To: <sip:1001@10.0.0.2>\r\n"
               "Contact: <sip:asterisk@10.0.0.1:5060>\r\n"
               "Call-ID: %s\r\n"
               "CSeq: %d INVITE\r\n"
               "%s"
               "Content-Type: application/sdp\r\n"
               "Content-Length: %u\r\n\r\n%s",
               branch, callid, cseq, extra ? extra : "", (unsigned)(sdp ? strlen(sdp) : 0), sdp ? sdp : "");
}

/* in-dialog request from Bob (incoming-call dialog) */
static void inject_from_bob(const char *method, int cseq, const char *callid, const char *our_tag,
                            const char *branch, const char *sdp)
{
    inject_fmt("%s sip:1001@10.0.0.2:5060 SIP/2.0\r\n"
               "Via: SIP/2.0/UDP 10.0.0.1:5060;rport;branch=%s\r\n"
               "Max-Forwards: 70\r\n"
               "From: \"Bob\" <sip:1002@10.0.0.1>;tag=remote1\r\n"
               "To: <sip:1001@10.0.0.2>;tag=%s\r\n"
               "Contact: <sip:asterisk@10.0.0.1:5060>\r\n"
               "Call-ID: %s\r\n"
               "CSeq: %d %s\r\n"
               "%s"
               "Content-Length: %u\r\n\r\n%s",
               method, branch, our_tag, callid, cseq, method,
               sdp ? "Content-Type: application/sdp\r\n" : "", (unsigned)(sdp ? strlen(sdp) : 0),
               sdp ? sdp : "");
}

static char tagbuf[80];
static const char *to_tag_of(int idx)
{
    const sip_msg_t *m = fx_parse(f, idx);
    TEST_ASSERT_TRUE(m->to.tag.len > 0 && m->to.tag.len < sizeof(tagbuf));
    memcpy(tagbuf, m->to.tag.p, m->to.tag.len);
    tagbuf[m->to.tag.len] = '\0';
    return tagbuf;
}

static char fromtagbuf[80];
static const char *from_tag_of(int idx)
{
    const sip_msg_t *m = fx_parse(f, idx);
    memcpy(fromtagbuf, m->from.tag.p, m->from.tag.len);
    fromtagbuf[m->from.tag.len] = '\0';
    return fromtagbuf;
}

static char cidbuf[128];
static const char *call_id_of(int idx)
{
    const sip_msg_t *m = fx_parse(f, idx);
    memcpy(cidbuf, m->call_id.p, m->call_id.len);
    cidbuf[m->call_id.len] = '\0';
    return cidbuf;
}

/* in-dialog request from the far end of an OUTGOING call */
static void inject_from_callee(const char *method, int cseq, int invite_idx, const char *their_tag,
                               const char *branch, const char *sdp)
{
    char cid[128], ours[80];
    snprintf(cid, sizeof(cid), "%s", call_id_of(invite_idx));
    snprintf(ours, sizeof(ours), "%s", from_tag_of(invite_idx));
    inject_fmt("%s sip:1001@10.0.0.2:5060 SIP/2.0\r\n"
               "Via: SIP/2.0/UDP 10.0.0.1:5060;rport;branch=%s\r\n"
               "From: <sip:600@10.0.0.1>;tag=%s\r\n"
               "To: \"Alice\" <sip:1001@10.0.0.1>;tag=%s\r\n"
               "Call-ID: %s\r\n"
               "CSeq: %d %s\r\n"
               "Contact: <sip:srv@10.0.0.1:5060>\r\n"
               "%s"
               "Content-Length: %u\r\n\r\n%s",
               method, branch, their_tag, ours, cid, cseq, method,
               sdp ? "Content-Type: application/sdp\r\n" : "", (unsigned)(sdp ? strlen(sdp) : 0),
               sdp ? sdp : "");
}

static const fx_ev_t *last_call_ev(void)
{
    const fx_ev_t *e = fx_last_ev(f, FXE_CALL);
    TEST_ASSERT_NOT_NULL(e);
    return e;
}

static void extract_param(const char *hdr, const char *name, char *out, size_t cap)
{
    char key[32];
    snprintf(key, sizeof(key), "%s=", name);
    const char *p = strstr(hdr, key);
    TEST_ASSERT_NOT_NULL_MESSAGE(p, name);
    p += strlen(key);
    if (*p == '"') {
        p++;
    }
    size_t n = 0;
    while (p[n] && p[n] != '"' && p[n] != ',' && n + 1 < cap) {
        n++;
    }
    memcpy(out, p, n);
    out[n] = '\0';
}

static void do_register(void)
{
    TEST_ASSERT_EQUAL(SIP_OK, sip_ua_register(f->ua));
    int r = fx_find(f, "REGISTER", 0);
    fx_reply(f, r, 401, "s1", CHALLENGE, NULL);
    r = fx_find(f, "REGISTER", 0);
    fx_reply(f, r, 200, "s1", "Contact: <sip:1001@10.0.0.2:5060>;expires=300\r\n", NULL);
    TEST_ASSERT_EQUAL(SIP_UA_REG_REGISTERED, sip_ua_reg_state(f->ua));
}

/* outgoing call to 600 up to ACTIVE; returns INVITE index */
static int establish_outgoing(sip_call_handle_t *h)
{
    TEST_ASSERT_EQUAL(SIP_OK, sip_call_dial(f->ua, "600", h));
    int inv = fx_find(f, "INVITE", 0);
    fx_reply(f, inv, 100, NULL, NULL, NULL);
    fx_reply(f, inv, 180, "callee", NULL, NULL);
    fx_reply(f, inv, 200, "callee", NULL, FX_SDP_PCMU);
    TEST_ASSERT_EQUAL(SIP_UA_CALL_ACTIVE, sip_call_state(f->ua, *h));
    return inv;
}

/* incoming call answered + ACKed; returns our To tag via tagbuf */
static sip_call_handle_t establish_incoming(const char *callid, const char **our_tag)
{
    inject_invite(callid, "z9hG4bKinv1", 100, FX_SDP_PCMU, NULL);
    const fx_ev_t *in = fx_last_ev(f, FXE_INCOMING);
    TEST_ASSERT_NOT_NULL(in);
    sip_call_handle_t h = in->call;
    TEST_ASSERT_EQUAL(SIP_OK, sip_call_answer(f->ua, h));
    int ok = fx_find(f, "SIP/2.0 200", 0);
    *our_tag = to_tag_of(ok);
    inject_from_bob("ACK", 100, callid, *our_tag, "z9hG4bKack1", NULL);
    return h;
}

/* ------------------------------------------------------- registration */

static void test_register_401_then_200(void)
{
    fx_init(f, NULL);
    TEST_ASSERT_EQUAL(SIP_OK, sip_ua_register(f->ua));
    TEST_ASSERT_EQUAL(1, f->n_sent);
    const sip_msg_t *m = fx_parse(f, 0);
    TEST_ASSERT_EQUAL(SIP_METHOD_REGISTER, m->method);
    TEST_ASSERT_EQUAL(300, m->expires);
    TEST_ASSERT_EQUAL_UINT32(1, m->cseq);
    TEST_ASSERT_NULL(fx_hdr(f, 0, "Authorization"));
    TEST_ASSERT_EQUAL_STRING("sipwire/0.1", fx_hdr(f, 0, "User-Agent"));
    TEST_ASSERT_EQUAL_STRING("<sip:1001@10.0.0.2:5060>", fx_hdr(f, 0, "Contact"));
    TEST_ASSERT_EQUAL_STRING("\"Alice\" <sip:1001@10.0.0.1>", fx_hdr(f, 0, "To"));
    TEST_ASSERT_EQUAL_HEX32(FX_SERVER_IP, f->sent[0].to.ip);
    TEST_ASSERT_EQUAL(5060, f->sent[0].to.port);
    TEST_ASSERT_EQUAL(SIP_UA_REG_REGISTERING, fx_last_ev(f, FXE_REG)->state);
    char cid[128];
    snprintf(cid, sizeof(cid), "%s", call_id_of(0));

    fx_reply(f, 0, 401, "s1", CHALLENGE, NULL);
    TEST_ASSERT_EQUAL(2, f->n_sent);
    m = fx_parse(f, 1);
    TEST_ASSERT_EQUAL_UINT32(2, m->cseq);
    TEST_ASSERT_EQUAL_STRING(cid, call_id_of(1));
    const char *auth = fx_hdr(f, 1, "Authorization");
    TEST_ASSERT_NOT_NULL(auth);
    char cnonce[40], nc[16], uri[64], resp[40], exp[33];
    extract_param(auth, "cnonce", cnonce, sizeof(cnonce));
    extract_param(auth, "nc", nc, sizeof(nc));
    extract_param(auth, "uri", uri, sizeof(uri));
    extract_param(auth, "response", resp, sizeof(resp));
    TEST_ASSERT_EQUAL_STRING("00000001", nc);
    TEST_ASSERT_EQUAL_STRING("sip:10.0.0.1", uri);
    sip_digest_response("1001", "asterisk", "secret", "REGISTER", uri, "n1", nc, cnonce, "auth", exp);
    TEST_ASSERT_EQUAL_STRING(exp, resp);

    fx_reply(f, 1, 200, "s1", "Contact: <sip:1001@10.0.0.2:5060>;expires=120\r\n", NULL);
    const fx_ev_t *e = fx_last_ev(f, FXE_REG);
    TEST_ASSERT_EQUAL(SIP_UA_REG_REGISTERED, e->state);
    TEST_ASSERT_EQUAL(200, e->code);
    TEST_ASSERT_EQUAL(2, fx_count_ev(f, FXE_REG));
    /* retransmitted 200 is absorbed by the transaction */
    fx_reply(f, 1, 200, "s1", "Contact: <sip:1001@10.0.0.2:5060>;expires=120\r\n", NULL);
    TEST_ASSERT_EQUAL(2, f->n_sent);
}

static void test_register_refresh_timing(void)
{
    fx_init(f, NULL);
    sip_ua_register(f->ua);
    fx_reply(f, 0, 401, "s1", CHALLENGE, NULL);
    /* server grants less than requested: honour it (refresh = min(e/2, e-30)) */
    fx_reply(f, 1, 200, "s1", "Contact: <sip:1001@10.0.0.2:5060>;expires=120\r\n", NULL);
    fx_advance(f, 59990);
    TEST_ASSERT_EQUAL(2, fx_count(f, "REGISTER"));
    fx_advance(f, 20);
    TEST_ASSERT_EQUAL(3, fx_count(f, "REGISTER"));
    int r = fx_find(f, "REGISTER", 0);
    const char *auth = fx_hdr(f, r, "Authorization");
    TEST_ASSERT_NOT_NULL(auth); /* cached credentials, nonce reused */
    TEST_ASSERT_NOT_NULL(strstr(auth, "nc=00000002"));
    /* Expires header form, 50 s → refresh after 20 s (e - 30) */
    fx_reply(f, r, 200, "s1", "Expires: 50\r\n", NULL);
    int nreg = fx_count_ev(f, FXE_REG);
    fx_advance(f, 19990);
    TEST_ASSERT_EQUAL(3, fx_count(f, "REGISTER"));
    fx_advance(f, 20);
    TEST_ASSERT_EQUAL(4, fx_count(f, "REGISTER"));
    TEST_ASSERT_EQUAL(nreg, fx_count_ev(f, FXE_REG)); /* refreshes don't re-report */
    /* stale nonce on refresh: retry immediately with the new nonce */
    r = fx_find(f, "REGISTER", 0);
    fx_reply(f, r, 401, "s2",
             "WWW-Authenticate: Digest realm=\"asterisk\",nonce=\"n2\",qop=\"auth\",stale=true\r\n", NULL);
    TEST_ASSERT_EQUAL(5, fx_count(f, "REGISTER"));
    r = fx_find(f, "REGISTER", 0);
    TEST_ASSERT_NOT_NULL(strstr(fx_hdr(f, r, "Authorization"), "nonce=\"n2\""));
    fx_reply(f, r, 200, "s1", "Expires: 300\r\n", NULL);
    TEST_ASSERT_EQUAL(SIP_UA_REG_REGISTERED, sip_ua_reg_state(f->ua));
    fx_advance(f, 150000);
    TEST_ASSERT_EQUAL(6, fx_count(f, "REGISTER"));
}

static void test_register_wrong_password(void)
{
    fx_init(f, NULL);
    sip_ua_register(f->ua);
    fx_reply(f, 0, 401, "s1", CHALLENGE, NULL);
    fx_reply(f, 1, 401, "s1",
             "WWW-Authenticate: Digest realm=\"asterisk\",nonce=\"n9\",qop=\"auth\"\r\n", NULL);
    TEST_ASSERT_EQUAL(2, f->n_sent); /* no third attempt */
    const fx_ev_t *e = fx_last_ev(f, FXE_REG);
    TEST_ASSERT_EQUAL(SIP_UA_REG_FAILED, e->state);
    TEST_ASSERT_EQUAL(401, e->code);
}

static void test_register_failure_backoff(void)
{
    fx_init(f, NULL);
    sip_ua_register(f->ua);
    fx_reply(f, 0, 403, "s1", NULL, NULL);
    TEST_ASSERT_EQUAL(SIP_UA_REG_FAILED, fx_last_ev(f, FXE_REG)->state);
    TEST_ASSERT_EQUAL(403, fx_last_ev(f, FXE_REG)->code);
    fx_advance(f, 4990);
    TEST_ASSERT_EQUAL(1, f->n_sent);
    fx_advance(f, 20);
    TEST_ASSERT_EQUAL(2, f->n_sent);
    TEST_ASSERT_EQUAL(SIP_UA_REG_REGISTERING, fx_last_ev(f, FXE_REG)->state);
    fx_reply(f, 1, 503, "s1", NULL, NULL);
    fx_advance(f, 9990);
    TEST_ASSERT_EQUAL(2, f->n_sent);
    fx_advance(f, 20);
    TEST_ASSERT_EQUAL(3, f->n_sent); /* backoff doubled */
    fx_reply(f, 2, 200, "s1", "Expires: 300\r\n", NULL);
    TEST_ASSERT_EQUAL(SIP_UA_REG_REGISTERED, sip_ua_reg_state(f->ua));
}

static void test_register_timer_f(void)
{
    fx_init(f, NULL);
    sip_ua_register(f->ua);
    /* timer E: 0.5, 1, 2, 4, 4, 4... until timer F (32 s) */
    fx_advance(f, 31990);
    TEST_ASSERT_EQUAL(11, fx_count(f, "REGISTER"));
    TEST_ASSERT_EQUAL(SIP_UA_REG_REGISTERING, sip_ua_reg_state(f->ua));
    fx_advance(f, 20);
    TEST_ASSERT_EQUAL(SIP_UA_REG_FAILED, sip_ua_reg_state(f->ua));
    TEST_ASSERT_EQUAL(0, fx_last_ev(f, FXE_REG)->code);
    TEST_ASSERT_EQUAL(0, sip_ua_debug_txn_count(f->ua));
}

static void test_register_423(void)
{
    fx_init(f, NULL);
    sip_ua_register(f->ua);
    fx_reply(f, 0, 423, "s1", "Min-Expires: 600\r\n", NULL);
    TEST_ASSERT_EQUAL(2, f->n_sent);
    TEST_ASSERT_EQUAL(600, fx_parse(f, 1)->expires);
}

static void test_register_nat_rport(void)
{
    fx_init(f, NULL);
    sip_ua_register(f->ua);
    const sip_msg_t *m = fx_parse(f, 0);
    TEST_ASSERT_EQUAL(0, m->via[0].rport); /* we ask for rport */
    inject_fmt("SIP/2.0 200 OK\r\n"
               "Via: SIP/2.0/UDP 10.0.0.2:5060;rport=40000;received=203.0.113.5;branch=%.*s\r\n"
               "From: %s\r\nTo: <sip:1001@10.0.0.1>;tag=x\r\nCall-ID: %s\r\nCSeq: 1 REGISTER\r\n"
               "Contact: <sip:1001@10.0.0.2:5060>;expires=300\r\nContent-Length: 0\r\n\r\n",
               (int)m->via[0].branch.len, m->via[0].branch.p, fx_hdr(f, 0, "From"), call_id_of(0));
    TEST_ASSERT_EQUAL(2, fx_count(f, "REGISTER"));
    TEST_ASSERT_EQUAL_STRING("<sip:1001@203.0.113.5:40000>", fx_hdr(f, 1, "Contact"));
    sip_addr_t c = sip_ua_debug_contact(f->ua);
    TEST_ASSERT_EQUAL_HEX32(0xcb007105, c.ip);
    TEST_ASSERT_EQUAL(40000, c.port);
    TEST_ASSERT_EQUAL(SIP_UA_REG_REGISTERED, sip_ua_reg_state(f->ua));
    /* the second response confirms the same address: no loop */
    inject_fmt("SIP/2.0 200 OK\r\n"
               "Via: %s\r\n"
               "From: %s\r\nTo: <sip:1001@10.0.0.1>;tag=x\r\nCall-ID: %s\r\nCSeq: 2 REGISTER\r\n"
               "Contact: <sip:1001@203.0.113.5:40000>;expires=300\r\nContent-Length: 0\r\n\r\n",
               "SIP/2.0/UDP 10.0.0.2:5060;rport=40000;received=203.0.113.5;branch=PLACEHOLDER",
               fx_hdr(f, 1, "From"), call_id_of(1));
    /* (branch above does not match; deliver the real one) */
    m = fx_parse(f, 1);
    char br[64];
    snprintf(br, sizeof(br), "%.*s", (int)m->via[0].branch.len, m->via[0].branch.p);
    inject_fmt("SIP/2.0 200 OK\r\n"
               "Via: SIP/2.0/UDP 10.0.0.2:5060;rport=40000;received=203.0.113.5;branch=%s\r\n"
               "From: %s\r\nTo: <sip:1001@10.0.0.1>;tag=x\r\nCall-ID: %s\r\nCSeq: 2 REGISTER\r\n"
               "Contact: <sip:1001@203.0.113.5:40000>;expires=300\r\nContent-Length: 0\r\n\r\n",
               br, fx_hdr(f, 1, "From"), call_id_of(1));
    TEST_ASSERT_EQUAL(2, fx_count(f, "REGISTER"));
    /* SDP of a later call advertises the public address */
    sip_call_handle_t h;
    TEST_ASSERT_EQUAL(SIP_OK, sip_call_dial(f->ua, "600", &h));
    int inv = fx_find(f, "INVITE", 0);
    TEST_ASSERT_NOT_NULL(strstr(f->sent[inv].buf, "c=IN IP4 203.0.113.5\r\n"));
}

static void test_unregister(void)
{
    fx_init(f, NULL);
    do_register();
    int n0 = f->n_sent;
    TEST_ASSERT_EQUAL(SIP_OK, sip_ua_unregister(f->ua));
    TEST_ASSERT_EQUAL(n0 + 1, f->n_sent);
    int r = fx_find(f, "REGISTER", 0);
    TEST_ASSERT_EQUAL(0, fx_parse(f, r)->expires);
    TEST_ASSERT_NOT_NULL(fx_hdr(f, r, "Authorization"));
    fx_reply(f, r, 200, "s1", NULL, NULL);
    TEST_ASSERT_EQUAL(SIP_UA_REG_NONE, sip_ua_reg_state(f->ua));
    TEST_ASSERT_EQUAL(SIP_UA_REG_NONE, fx_last_ev(f, FXE_REG)->state);
    fx_advance(f, 400000);
    TEST_ASSERT_EQUAL(n0 + 1, f->n_sent); /* no refresh after unregister */
}

static void keepalive_crlf_cfg(sip_ua_config_t *c)
{
    c->keepalive_s = 15;
}

static void test_keepalive_crlf(void)
{
    fx_init(f, keepalive_crlf_cfg);
    do_register();
    int n0 = f->n_sent;
    fx_advance(f, 15010);
    TEST_ASSERT_EQUAL(n0 + 1, f->n_sent);
    TEST_ASSERT_EQUAL_STRING("\r\n\r\n", f->sent[n0].buf);
    /* a CRLF pong from the server is silently ignored */
    fx_inject(f, "\r\n");
    fx_advance(f, 15000);
    TEST_ASSERT_EQUAL(n0 + 2, f->n_sent);
}

static void keepalive_opt_cfg(sip_ua_config_t *c)
{
    c->keepalive_s = 20;
    c->keepalive_mode = SIP_KEEPALIVE_OPTIONS;
}

static void test_keepalive_options_timeout_reregisters(void)
{
    fx_init(f, keepalive_opt_cfg);
    do_register();
    fx_advance(f, 20010);
    int o = fx_find(f, "OPTIONS", 0);
    TEST_ASSERT_TRUE(o >= 0);
    fx_reply(f, o, 200, "x", NULL, NULL);
    int nreg = fx_count(f, "REGISTER");
    fx_advance(f, 20000);
    TEST_ASSERT_EQUAL(2, fx_count(f, "OPTIONS"));
    /* unanswered → timer F → re-register */
    fx_advance(f, 32100);
    TEST_ASSERT_EQUAL(nreg + 1, fx_count(f, "REGISTER"));
}

/* ------------------------------------------------------- outgoing calls */

static void test_outgoing_call_success(void)
{
    fx_init(f, NULL);
    sip_call_handle_t h = 0;
    TEST_ASSERT_EQUAL(SIP_OK, sip_call_dial(f->ua, "600", &h));
    TEST_ASSERT_TRUE(h > 0);
    TEST_ASSERT_EQUAL(SIP_UA_CALL_DIALING, last_call_ev()->state);
    int inv = fx_find(f, "INVITE", 0);
    const sip_msg_t *m = fx_parse(f, inv);
    TEST_ASSERT_EQUAL_STRING("INVITE sip:600@10.0.0.1 SIP/2.0", strtok(strcpy(sbuf, f->sent[inv].buf), "\r"));
    TEST_ASSERT_TRUE(m->body.len > 0);
    TEST_ASSERT_NOT_NULL(strstr(f->sent[inv].buf, "m=audio 4000 RTP/AVP 9 0 8 101\r\n"));
    TEST_ASSERT_NOT_NULL(strstr(f->sent[inv].buf, "a=rtpmap:9 G722/8000\r\n"));
    TEST_ASSERT_EQUAL_STRING("<sip:600@10.0.0.1>", fx_hdr(f, inv, "To"));

    fx_reply(f, inv, 100, NULL, NULL, NULL);
    TEST_ASSERT_EQUAL(SIP_UA_CALL_DIALING, sip_call_state(f->ua, h));
    fx_reply(f, inv, 180, "callee", NULL, NULL);
    TEST_ASSERT_EQUAL(SIP_UA_CALL_RINGING_OUT, last_call_ev()->state);
    TEST_ASSERT_EQUAL(180, last_call_ev()->code);
    fx_reply(f, inv, 200, "callee", "Record-Route: <sip:10.0.0.1;lr>\r\n", FX_SDP_PCMU);
    TEST_ASSERT_EQUAL(SIP_UA_CALL_ACTIVE, last_call_ev()->state);
    int ack = fx_find(f, "ACK", 0);
    TEST_ASSERT_TRUE(ack >= 0);
    m = fx_parse(f, ack);
    TEST_ASSERT_EQUAL_STRING("ACK sip:srv@10.0.0.1:5060 SIP/2.0", strtok(strcpy(sbuf, f->sent[ack].buf), "\r"));
    TEST_ASSERT_EQUAL_UINT32(1, m->cseq);
    TEST_ASSERT_EQUAL(SIP_METHOD_ACK, m->cseq_method);
    TEST_ASSERT_EQUAL_STRING("<sip:10.0.0.1;lr>", fx_hdr(f, ack, "Route"));
    TEST_ASSERT_TRUE(strstr(fx_hdr(f, ack, "To"), "tag=callee") != NULL);
    const fx_ev_t *me = fx_last_ev(f, FXE_MEDIA);
    TEST_ASSERT_NOT_NULL(me);
    TEST_ASSERT_EQUAL_STRING("10.0.0.1", me->media.remote_ip_str);
    TEST_ASSERT_EQUAL(20000, me->media.remote_port);
    TEST_ASSERT_EQUAL(4000, me->media.local_port);
    TEST_ASSERT_EQUAL(0, me->media.payload_type);
    TEST_ASSERT_EQUAL(101, me->media.dtmf_pt);
    TEST_ASSERT_EQUAL(20, me->media.ptime_ms);
    TEST_ASSERT_EQUAL(SIP_DIR_SENDRECV, me->media.direction);
    TEST_ASSERT_FALSE(me->media.early);

    /* 2xx retransmission (our ACK got lost) → ACK again, no new events */
    int nev = f->n_ev;
    fx_reply(f, inv, 200, "callee", "Record-Route: <sip:10.0.0.1;lr>\r\n", FX_SDP_PCMU);
    TEST_ASSERT_EQUAL(2, fx_count(f, "ACK"));
    TEST_ASSERT_EQUAL(nev, f->n_ev);

    TEST_ASSERT_EQUAL(SIP_OK, sip_call_hangup(f->ua, h));
    TEST_ASSERT_EQUAL(SIP_UA_CALL_ENDED, last_call_ev()->state);
    TEST_ASSERT_EQUAL(SIP_END_LOCAL, last_call_ev()->reason);
    int bye = fx_find(f, "BYE", 0);
    m = fx_parse(f, bye);
    TEST_ASSERT_EQUAL_UINT32(2, m->cseq);
    TEST_ASSERT_EQUAL_STRING("<sip:10.0.0.1;lr>", fx_hdr(f, bye, "Route"));
    fx_reply(f, bye, 200, NULL, NULL, NULL);
    TEST_ASSERT_EQUAL(SIP_UA_CALL_IDLE, sip_call_state(f->ua, h));
    fx_advance(f, 40000);
    TEST_ASSERT_EQUAL(0, sip_ua_debug_txn_count(f->ua));
    TEST_ASSERT_EQUAL(0, sip_ua_debug_call_slots(f->ua));
}

static void test_outgoing_with_proxy_auth(void)
{
    fx_init(f, NULL);
    sip_call_handle_t h;
    sip_call_dial(f->ua, "600", &h);
    int inv = fx_find(f, "INVITE", 0);
    fx_reply(f, inv, 407, "p1",
             "Proxy-Authenticate: Digest realm=\"asterisk\",nonce=\"pn\",qop=\"auth\"\r\n", NULL);
    /* the transaction ACKs the 407 with the INVITE's branch */
    int ack = fx_find(f, "ACK", 0);
    TEST_ASSERT_TRUE(ack >= 0);
    const sip_msg_t *im = fx_parse(f, inv);
    char br[64];
    snprintf(br, sizeof(br), "%.*s", (int)im->via[0].branch.len, im->via[0].branch.p);
    const sip_msg_t *am = fx_parse(f, ack);
    TEST_ASSERT_EQUAL_STRING_LEN(br, am->via[0].branch.p, am->via[0].branch.len);
    TEST_ASSERT_TRUE(strstr(fx_hdr(f, ack, "To"), "tag=p1") != NULL);
    int inv2 = fx_find(f, "INVITE", 0);
    TEST_ASSERT_TRUE(inv2 > inv);
    TEST_ASSERT_EQUAL_UINT32(2, fx_parse(f, inv2)->cseq);
    TEST_ASSERT_NOT_NULL(fx_hdr(f, inv2, "Proxy-Authorization"));
    TEST_ASSERT_TRUE(strstr(fx_hdr(f, inv2, "Proxy-Authorization"), "uri=\"sip:600@10.0.0.1\"") != NULL);
    TEST_ASSERT_EQUAL(SIP_UA_CALL_DIALING, sip_call_state(f->ua, h));
    fx_reply(f, inv2, 200, "callee", NULL, FX_SDP_PCMA_ONLY);
    TEST_ASSERT_EQUAL(SIP_UA_CALL_ACTIVE, sip_call_state(f->ua, h));
    TEST_ASSERT_EQUAL(8, fx_last_ev(f, FXE_MEDIA)->media.payload_type);
    TEST_ASSERT_EQUAL(-1, fx_last_ev(f, FXE_MEDIA)->media.dtmf_pt);
}

static void test_outgoing_busy(void)
{
    fx_init(f, NULL);
    sip_call_handle_t h;
    sip_call_dial(f->ua, "601", &h);
    int inv = fx_find(f, "INVITE", 0);
    fx_reply(f, inv, 180, "b", NULL, NULL);
    fx_reply(f, inv, 486, "b", NULL, NULL);
    TEST_ASSERT_EQUAL(1, fx_count(f, "ACK"));
    const fx_ev_t *e = last_call_ev();
    TEST_ASSERT_EQUAL(SIP_UA_CALL_ENDED, e->state);
    TEST_ASSERT_EQUAL(SIP_END_REJECTED, e->reason);
    TEST_ASSERT_EQUAL(486, e->code);
    TEST_ASSERT_EQUAL_STRING("601", e->number);
    int nev = f->n_ev;
    fx_reply(f, inv, 486, "b", NULL, NULL); /* retransmitted final */
    TEST_ASSERT_EQUAL(2, fx_count(f, "ACK"));
    TEST_ASSERT_EQUAL(nev, f->n_ev);
    fx_advance(f, 33000); /* timer D */
    TEST_ASSERT_EQUAL(0, sip_ua_debug_txn_count(f->ua));
}

/* A burst of short calls leaves many INVITE transactions lingering in
 * Completed (timer D, 32 s); new requests must still get a transaction. */
static void test_txn_pool_burst(void)
{
    fx_init(f, NULL);
    for (int i = 0; i < 20; i++) {
        sip_call_handle_t h;
        fx_clear_sent(f);
        TEST_ASSERT_EQUAL(SIP_OK, sip_call_dial(f->ua, "601", &h));
        int inv = fx_find(f, "INVITE", 0);
        TEST_ASSERT_TRUE(inv >= 0);
        fx_reply(f, inv, 486, "b", NULL, NULL);
        TEST_ASSERT_EQUAL(SIP_UA_CALL_ENDED, last_call_ev()->state);
        TEST_ASSERT_EQUAL(1, fx_count(f, "ACK"));
        fx_advance(f, 100);
    }
    sip_call_handle_t h;
    fx_clear_sent(f);
    TEST_ASSERT_EQUAL(SIP_OK, sip_call_dial(f->ua, "600", &h));
    TEST_ASSERT_EQUAL(1, fx_count(f, "INVITE"));
}

static void test_outgoing_timer_b(void)
{
    fx_init(f, NULL);
    sip_call_handle_t h;
    sip_call_dial(f->ua, "600", &h);
    /* timer A doubles: 0, .5, 1.5, 3.5, 7.5, 15.5, 31.5 */
    fx_advance(f, 31990);
    TEST_ASSERT_EQUAL(7, fx_count(f, "INVITE"));
    TEST_ASSERT_EQUAL(SIP_UA_CALL_DIALING, sip_call_state(f->ua, h));
    fx_advance(f, 20);
    const fx_ev_t *e = last_call_ev();
    TEST_ASSERT_EQUAL(SIP_UA_CALL_ENDED, e->state);
    TEST_ASSERT_EQUAL(SIP_END_TIMEOUT, e->reason);
    TEST_ASSERT_EQUAL(408, e->code);
    TEST_ASSERT_EQUAL(0, sip_ua_debug_call_slots(f->ua));
}

static void test_cancel_early(void)
{
    fx_init(f, NULL);
    sip_call_handle_t h;
    sip_call_dial(f->ua, "600", &h);
    int inv = fx_find(f, "INVITE", 0);
    fx_reply(f, inv, 180, "c1", NULL, NULL);
    TEST_ASSERT_EQUAL(SIP_OK, sip_call_hangup(f->ua, h));
    TEST_ASSERT_EQUAL(SIP_UA_CALL_ENDED, last_call_ev()->state);
    TEST_ASSERT_EQUAL(SIP_END_LOCAL, last_call_ev()->reason);
    int can = fx_find(f, "CANCEL", 0);
    TEST_ASSERT_TRUE(can >= 0);
    const sip_msg_t *cm = fx_parse(f, can);
    TEST_ASSERT_EQUAL(SIP_METHOD_CANCEL, cm->cseq_method);
    TEST_ASSERT_EQUAL_UINT32(1, cm->cseq);
    TEST_ASSERT_EQUAL_STRING("<sip:600@10.0.0.1>", fx_hdr(f, can, "To")); /* no tag (§9.1) */
    char br[64];
    snprintf(br, sizeof(br), "%.*s", (int)cm->via[0].branch.len, cm->via[0].branch.p);
    const sip_msg_t *im = fx_parse(f, inv);
    TEST_ASSERT_EQUAL_STRING_LEN(br, im->via[0].branch.p, im->via[0].branch.len);
    fx_reply(f, can, 200, "c1", NULL, NULL);
    fx_reply(f, inv, 487, "c1", NULL, NULL);
    TEST_ASSERT_EQUAL(1, fx_count(f, "ACK"));
    TEST_ASSERT_EQUAL(3, fx_count_ev(f, FXE_CALL)); /* DIALING, RINGING_OUT, ENDED */
    TEST_ASSERT_EQUAL(0, sip_ua_debug_call_slots(f->ua));
}

static void test_cancel_before_provisional(void)
{
    fx_init(f, NULL);
    sip_call_handle_t h;
    sip_call_dial(f->ua, "600", &h);
    sip_call_hangup(f->ua, h);
    TEST_ASSERT_EQUAL(0, fx_count(f, "CANCEL")); /* §9.1: wait for a provisional */
    TEST_ASSERT_EQUAL(SIP_UA_CALL_ENDED, last_call_ev()->state);
    int inv = fx_find(f, "INVITE", 0);
    fx_reply(f, inv, 100, NULL, NULL, NULL);
    TEST_ASSERT_EQUAL(1, fx_count(f, "CANCEL"));
    /* the user can dial again while the old call is cleaned up */
    sip_call_handle_t h2;
    TEST_ASSERT_EQUAL(SIP_OK, sip_call_dial(f->ua, "601", &h2));
    TEST_ASSERT_NOT_EQUAL(h, h2);
}

static void test_cancel_200_race(void)
{
    fx_init(f, NULL);
    sip_call_handle_t h;
    sip_call_dial(f->ua, "600", &h);
    int inv = fx_find(f, "INVITE", 0);
    fx_reply(f, inv, 183, "r1", NULL, FX_SDP_PCMU);
    TEST_ASSERT_TRUE(fx_last_ev(f, FXE_MEDIA)->media.early);
    sip_call_hangup(f->ua, h);
    TEST_ASSERT_EQUAL(1, fx_count(f, "CANCEL"));
    int nev = f->n_ev;
    /* the 200 crossed our CANCEL: ACK it, then BYE */
    fx_reply(f, inv, 200, "r1", NULL, FX_SDP_PCMU);
    fx_reply(f, fx_find(f, "CANCEL", 0), 200, "r1", NULL, NULL);
    TEST_ASSERT_EQUAL(1, fx_count(f, "ACK"));
    int bye = fx_find(f, "BYE", 0);
    TEST_ASSERT_TRUE(bye >= 0);
    TEST_ASSERT_TRUE(strstr(fx_hdr(f, bye, "To"), "tag=r1") != NULL);
    TEST_ASSERT_EQUAL(nev, f->n_ev);
    fx_reply(f, bye, 200, NULL, NULL, NULL);
    fx_advance(f, 40000);
    TEST_ASSERT_EQUAL(0, sip_ua_debug_txn_count(f->ua));
    TEST_ASSERT_EQUAL(0, sip_ua_debug_call_slots(f->ua));
}

static void test_outgoing_no_common_codec(void)
{
    fx_init(f, NULL);
    sip_call_handle_t h;
    sip_call_dial(f->ua, "600", &h);
    int inv = fx_find(f, "INVITE", 0);
    fx_reply(f, inv, 200, "x", NULL, FX_SDP_G729_ONLY);
    TEST_ASSERT_EQUAL(1, fx_count(f, "ACK"));
    TEST_ASSERT_EQUAL(1, fx_count(f, "BYE"));
    TEST_ASSERT_EQUAL(SIP_END_ERROR, last_call_ev()->reason);
    TEST_ASSERT_EQUAL(488, last_call_ev()->code);
}

static void test_remote_bye_outgoing(void)
{
    fx_init(f, NULL);
    sip_call_handle_t h;
    int inv = establish_outgoing(&h);
    inject_from_callee("BYE", 5, inv, "callee", "z9hG4bKbye1", NULL);
    int ok = fx_find(f, "SIP/2.0 200", 0);
    TEST_ASSERT_TRUE(ok >= 0);
    TEST_ASSERT_EQUAL(SIP_METHOD_BYE, fx_parse(f, ok)->cseq_method);
    TEST_ASSERT_EQUAL(SIP_UA_CALL_ENDED, last_call_ev()->state);
    TEST_ASSERT_EQUAL(SIP_END_REMOTE, last_call_ev()->reason);
    /* retransmitted BYE: 200 again from the transaction, no second event */
    int nev = f->n_ev;
    inject_from_callee("BYE", 5, inv, "callee", "z9hG4bKbye1", NULL);
    TEST_ASSERT_EQUAL(2, fx_count(f, "SIP/2.0 200"));
    TEST_ASSERT_EQUAL(nev, f->n_ev);
    /* response goes back to the source (rport), with rport filled in */
    const sip_msg_t *m = fx_parse(f, ok);
    TEST_ASSERT_EQUAL(5060, m->via[0].rport);
}

static void test_local_hold_resume(void)
{
    fx_init(f, NULL);
    sip_call_handle_t h;
    int inv = establish_outgoing(&h);
    (void)inv;
    TEST_ASSERT_EQUAL(SIP_OK, sip_call_hold(f->ua, h, true));
    int re = fx_find(f, "INVITE", 0);
    TEST_ASSERT_EQUAL_UINT32(2, fx_parse(f, re)->cseq);
    TEST_ASSERT_NOT_NULL(strstr(f->sent[re].buf, "a=sendonly\r\n"));
    TEST_ASSERT_NOT_NULL(strstr(f->sent[re].buf, "o=sipwire "));
    TEST_ASSERT_EQUAL(SIP_ERR_STATE, sip_call_hold(f->ua, h, false)); /* one at a time */
    const char *ans =
        "v=0\r\no=srv 1 2 IN IP4 10.0.0.1\r\ns=-\r\nc=IN IP4 10.0.0.1\r\nt=0 0\r\n"
        "m=audio 20000 RTP/AVP 0\r\na=recvonly\r\n";
    fx_reply(f, re, 200, "callee", NULL, ans);
    int ack = fx_find(f, "ACK", 0);
    TEST_ASSERT_EQUAL_UINT32(2, fx_parse(f, ack)->cseq);
    TEST_ASSERT_EQUAL(SIP_UA_CALL_HELD, sip_call_state(f->ua, h));
    TEST_ASSERT_EQUAL(SIP_UA_CALL_HELD, last_call_ev()->state);
    TEST_ASSERT_EQUAL(SIP_DIR_SENDONLY, fx_last_ev(f, FXE_MEDIA)->media.direction);
    TEST_ASSERT_EQUAL(SIP_OK, sip_call_hold(f->ua, h, false));
    re = fx_find(f, "INVITE", 0);
    TEST_ASSERT_NOT_NULL(strstr(f->sent[re].buf, "a=sendrecv\r\n"));
    fx_reply(f, re, 200, "callee", NULL, FX_SDP_PCMU);
    TEST_ASSERT_EQUAL(SIP_UA_CALL_ACTIVE, sip_call_state(f->ua, h));
    TEST_ASSERT_EQUAL(SIP_DIR_SENDRECV, fx_last_ev(f, FXE_MEDIA)->media.direction);
}

static void test_dial_rules(void)
{
    fx_init(f, NULL);
    sip_call_handle_t h, h2;
    TEST_ASSERT_EQUAL(SIP_OK, sip_call_dial(f->ua, "*97#", &h));
    int inv = fx_find(f, "INVITE", 0);
    TEST_ASSERT_EQUAL_STRING("<sip:*97%23@10.0.0.1>", fx_hdr(f, inv, "To"));
    TEST_ASSERT_EQUAL(SIP_ERR_BUSY, sip_call_dial(f->ua, "600", &h2));
    TEST_ASSERT_EQUAL(SIP_ERR_NOT_FOUND, sip_call_answer(f->ua, 999));
    TEST_ASSERT_EQUAL(SIP_ERR_STATE, sip_call_answer(f->ua, h));
    sip_call_hangup(f->ua, h);
    TEST_ASSERT_EQUAL(SIP_ERR_ARG, sip_call_dial(f->ua, "", &h2));
    TEST_ASSERT_EQUAL(SIP_ERR_ARG, sip_call_dial(f->ua, "a\nb", &h2));
}

/* ------------------------------------------------------- incoming calls */

static void test_incoming_answer(void)
{
    fx_init(f, NULL);
    inject_invite("in1@10.0.0.1", "z9hG4bKinv1", 100, FX_SDP_PCMU, "Record-Route: <sip:10.0.0.1;lr>\r\n");
    int t100 = fx_find(f, "SIP/2.0 100", 0);
    int t180 = fx_find(f, "SIP/2.0 180", 0);
    TEST_ASSERT_TRUE(t100 >= 0 && t180 > t100);
    TEST_ASSERT_EQUAL(0, fx_parse(f, t100)->to.tag.len);
    const char *tag = to_tag_of(t180);
    char our_tag[80];
    snprintf(our_tag, sizeof(our_tag), "%s", tag);
    TEST_ASSERT_EQUAL_STRING("<sip:1001@10.0.0.2:5060>", fx_hdr(f, t180, "Contact"));
    TEST_ASSERT_EQUAL_STRING("<sip:10.0.0.1;lr>", fx_hdr(f, t180, "Record-Route"));
    const fx_ev_t *in = fx_last_ev(f, FXE_INCOMING);
    TEST_ASSERT_NOT_NULL(in);
    TEST_ASSERT_EQUAL_STRING("1002", in->number);
    TEST_ASSERT_EQUAL_STRING("Bob", in->display);
    TEST_ASSERT_EQUAL(SIP_UA_CALL_RINGING_IN, last_call_ev()->state);
    sip_call_handle_t h = in->call;

    TEST_ASSERT_EQUAL(SIP_OK, sip_call_answer(f->ua, h));
    int ok = fx_find(f, "SIP/2.0 200", 0);
    TEST_ASSERT_EQUAL_STRING(our_tag, to_tag_of(ok));
    TEST_ASSERT_NOT_NULL(strstr(f->sent[ok].buf, "m=audio 4000 RTP/AVP 0 101\r\n"));
    TEST_ASSERT_NOT_NULL(strstr(f->sent[ok].buf, "a=sendrecv\r\n"));
    TEST_ASSERT_EQUAL(SIP_UA_CALL_ACTIVE, last_call_ev()->state);
    TEST_ASSERT_EQUAL(20000, fx_last_ev(f, FXE_MEDIA)->media.remote_port);

    /* 2xx retransmitted (T1, 2T1...) until the ACK arrives */
    fx_advance(f, 510);
    TEST_ASSERT_EQUAL(2, fx_count(f, "SIP/2.0 200"));
    fx_advance(f, 1000);
    TEST_ASSERT_EQUAL(3, fx_count(f, "SIP/2.0 200"));
    inject_from_bob("ACK", 100, "in1@10.0.0.1", our_tag, "z9hG4bKack1", NULL);
    fx_advance(f, 10000);
    TEST_ASSERT_EQUAL(3, fx_count(f, "SIP/2.0 200"));

    /* we hang up: BYE through the route set to the remote target */
    sip_call_hangup(f->ua, h);
    int bye = fx_find(f, "BYE", 0);
    TEST_ASSERT_EQUAL_STRING("BYE sip:asterisk@10.0.0.1:5060 SIP/2.0", strtok(strcpy(sbuf, f->sent[bye].buf), "\r"));
    TEST_ASSERT_EQUAL_STRING("<sip:10.0.0.1;lr>", fx_hdr(f, bye, "Route"));
    TEST_ASSERT_TRUE(strstr(fx_hdr(f, bye, "From"), our_tag) != NULL);
    TEST_ASSERT_TRUE(strstr(fx_hdr(f, bye, "To"), "tag=remote1") != NULL);
    fx_reply(f, bye, 200, NULL, NULL, NULL);
    fx_advance(f, 40000);
    TEST_ASSERT_EQUAL(0, sip_ua_debug_txn_count(f->ua));
}

static void test_incoming_retransmitted_invite(void)
{
    fx_init(f, NULL);
    inject_invite("in2@x", "z9hG4bKinv2", 1, FX_SDP_PCMU, NULL);
    inject_invite("in2@x", "z9hG4bKinv2", 1, FX_SDP_PCMU, NULL);
    TEST_ASSERT_EQUAL(1, fx_count_ev(f, FXE_INCOMING));
    TEST_ASSERT_EQUAL(2, fx_count(f, "SIP/2.0 180")); /* last provisional resent */
}

static void test_incoming_reject(void)
{
    fx_init(f, NULL);
    inject_invite("in3@x", "z9hG4bKinv3", 7, FX_SDP_PCMU, NULL);
    sip_call_handle_t h = fx_last_ev(f, FXE_INCOMING)->call;
    TEST_ASSERT_EQUAL(SIP_ERR_ARG, sip_call_reject(f->ua, h, 200));
    TEST_ASSERT_EQUAL(SIP_OK, sip_call_reject(f->ua, h, 603));
    TEST_ASSERT_EQUAL(SIP_UA_CALL_ENDED, last_call_ev()->state);
    TEST_ASSERT_EQUAL(SIP_END_LOCAL, last_call_ev()->reason);
    TEST_ASSERT_EQUAL(603, last_call_ev()->code);
    int r = fx_find(f, "SIP/2.0 603", 0);
    char tag[80];
    snprintf(tag, sizeof(tag), "%s", to_tag_of(r));
    TEST_ASSERT_EQUAL_STRING(tag, to_tag_of(fx_find(f, "SIP/2.0 180", 0)));
    /* timer G retransmits until the ACK (same branch) */
    fx_advance(f, 510);
    TEST_ASSERT_EQUAL(2, fx_count(f, "SIP/2.0 603"));
    inject_from_bob("ACK", 7, "in3@x", tag, "z9hG4bKinv3", NULL);
    fx_advance(f, 10000);
    TEST_ASSERT_EQUAL(2, fx_count(f, "SIP/2.0 603"));
    TEST_ASSERT_EQUAL(0, sip_ua_debug_txn_count(f->ua)); /* timer I = T4 */
}

static void test_incoming_reject_no_ack_timer_h(void)
{
    fx_init(f, NULL);
    inject_invite("in3b@x", "z9hG4bKinv3b", 7, FX_SDP_PCMU, NULL);
    sip_call_hangup(f->ua, fx_last_ev(f, FXE_INCOMING)->call); /* → 486 */
    TEST_ASSERT_EQUAL(1, fx_count(f, "SIP/2.0 486"));
    fx_advance(f, 31990);
    /* G: .5, 1.5, 3.5, 7.5, 11.5, 15.5, 19.5, 23.5, 27.5, 31.5 */
    TEST_ASSERT_EQUAL(11, fx_count(f, "SIP/2.0 486"));
    fx_advance(f, 20);
    TEST_ASSERT_EQUAL(0, sip_ua_debug_txn_count(f->ua));
}

static void test_incoming_remote_cancel(void)
{
    fx_init(f, NULL);
    inject_invite("in4@x", "z9hG4bKinv4", 3, FX_SDP_PCMU, NULL);
    sip_call_handle_t h = fx_last_ev(f, FXE_INCOMING)->call;
    inject_fmt("CANCEL sip:1001@10.0.0.2:5060 SIP/2.0\r\n"
               "Via: SIP/2.0/UDP 10.0.0.1:5060;rport;branch=z9hG4bKinv4\r\n"
               "From: \"Bob\" <sip:1002@10.0.0.1>;tag=remote1\r\n"
               "To: <sip:1001@10.0.0.2>\r\n"
               "Call-ID: in4@x\r\nCSeq: 3 CANCEL\r\nContent-Length: 0\r\n\r\n");
    int ok = fx_find(f, "SIP/2.0 200", 0);
    TEST_ASSERT_EQUAL(SIP_METHOD_CANCEL, fx_parse(f, ok)->cseq_method);
    int r487 = fx_find(f, "SIP/2.0 487", 0);
    TEST_ASSERT_TRUE(r487 >= 0);
    TEST_ASSERT_EQUAL(SIP_METHOD_INVITE, fx_parse(f, r487)->cseq_method);
    const fx_ev_t *e = last_call_ev();
    TEST_ASSERT_EQUAL(SIP_UA_CALL_ENDED, e->state);
    TEST_ASSERT_EQUAL(SIP_END_CANCELLED, e->reason);
    TEST_ASSERT_EQUAL(487, e->code);
    TEST_ASSERT_EQUAL(SIP_ERR_NOT_FOUND, sip_call_answer(f->ua, h));
    inject_from_bob("ACK", 3, "in4@x", to_tag_of(r487), "z9hG4bKinv4", NULL);
    /* CANCEL for an unknown transaction → 481 */
    inject_fmt("CANCEL sip:1001@10.0.0.2:5060 SIP/2.0\r\n"
               "Via: SIP/2.0/UDP 10.0.0.1:5060;branch=z9hG4bKnope\r\n"
               "From: <sip:1002@10.0.0.1>;tag=r\r\nTo: <sip:1001@10.0.0.2>\r\n"
               "Call-ID: zz\r\nCSeq: 1 CANCEL\r\nContent-Length: 0\r\n\r\n");
    TEST_ASSERT_TRUE(fx_find(f, "SIP/2.0 481", 0) >= 0);
}

static void test_incoming_busy_second_call(void)
{
    fx_init(f, NULL);
    const char *tag;
    establish_incoming("first@x", &tag);
    int nin = fx_count_ev(f, FXE_INCOMING);
    inject_fmt("INVITE sip:1001@10.0.0.2:5060 SIP/2.0\r\n"
               "Via: SIP/2.0/UDP 10.0.0.1:5060;branch=z9hG4bKsecond\r\n"
               "From: <sip:1003@10.0.0.1>;tag=c3\r\nTo: <sip:1001@10.0.0.2>\r\n"
               "Contact: <sip:x@10.0.0.1>\r\nCall-ID: second@x\r\nCSeq: 1 INVITE\r\n"
               "Content-Type: application/sdp\r\nContent-Length: %u\r\n\r\n%s",
               (unsigned)strlen(FX_SDP_PCMU), FX_SDP_PCMU);
    int busy = fx_find(f, "SIP/2.0 486", 0);
    TEST_ASSERT_TRUE(busy >= 0);
    TEST_ASSERT_EQUAL_STRING("second@x", call_id_of(busy));
    TEST_ASSERT_EQUAL(nin, fx_count_ev(f, FXE_INCOMING));
    TEST_ASSERT_EQUAL(1, sip_ua_debug_call_slots(f->ua));
}

static void test_outgoing_g722_answer(void)
{
    fx_init(f, NULL);
    sip_call_handle_t h;
    sip_call_dial(f->ua, "600", &h);
    int inv = fx_find(f, "INVITE", 0);
    fx_reply(f, inv, 200, "callee", NULL, FX_SDP_G722);
    const fx_ev_t *me = fx_last_ev(f, FXE_MEDIA);
    TEST_ASSERT_NOT_NULL(me);
    TEST_ASSERT_EQUAL(9, me->media.payload_type);
    TEST_ASSERT_EQUAL(101, me->media.dtmf_pt);
    TEST_ASSERT_EQUAL(20006, me->media.remote_port);
    TEST_ASSERT_EQUAL(SIP_UA_CALL_ACTIVE, sip_call_state(f->ua, h));
}

static void test_incoming_g722_offer_answered_in_pref_order(void)
{
    fx_init(f, NULL);
    inject_invite("g1@x", "z9hG4bKg1", 1, FX_SDP_G722, NULL);
    sip_call_handle_t h = fx_last_ev(f, FXE_INCOMING)->call;
    TEST_ASSERT_EQUAL(SIP_OK, sip_call_answer(f->ua, h));
    int ok = fx_find(f, "SIP/2.0 200", 0);
    TEST_ASSERT_NOT_NULL(strstr(f->sent[ok].buf, "m=audio 4000 RTP/AVP 9 101\r\n"));
    TEST_ASSERT_NOT_NULL(strstr(f->sent[ok].buf, "a=rtpmap:9 G722/8000\r\n"));
    TEST_ASSERT_EQUAL(9, fx_last_ev(f, FXE_MEDIA)->media.payload_type);
}

static int bad_send(void *ctx, const sip_addr_t *to, const void *data, size_t len)
{
    (void)ctx;
    (void)to;
    (void)data;
    (void)len;
    return 0;
}

static uint32_t bad_rand(void *ctx)
{
    (void)ctx;
    return 4;
}

static void tweak_narrowband_first(sip_ua_config_t *c)
{
    c->codecs[0] = 0;
    c->codecs[1] = 9;
    c->n_codecs = 2;
}

static void test_codec_preference_config(void)
{
    fx_init(f, tweak_narrowband_first);
    sip_call_handle_t h;
    sip_call_dial(f->ua, "600", &h);
    int inv = fx_find(f, "INVITE", 0);
    TEST_ASSERT_NOT_NULL(strstr(f->sent[inv].buf, "m=audio 4000 RTP/AVP 0 9 101\r\n"));
    TEST_ASSERT_NULL(strstr(f->sent[inv].buf, "PCMA"));
    fx_free(f);

    /* incoming G.722-first offer: our order picks PCMU */
    fx_init(f, tweak_narrowband_first);
    inject_invite("g2@x", "z9hG4bKg2", 1, FX_SDP_G722, NULL);
    sip_call_answer(f->ua, fx_last_ev(f, FXE_INCOMING)->call);
    TEST_ASSERT_EQUAL(0, fx_last_ev(f, FXE_MEDIA)->media.payload_type);
    fx_free(f);

    /* invalid lists are rejected */
    static const uint8_t bad[][SIP_UA_MAX_CODECS] = { {18}, {9, 9}, {0, 8, 0} };
    static const uint8_t n_bad[] = { 1, 2, 3 };
    for (size_t i = 0; i < sizeof(n_bad); i++) {
        sip_ua_config_t c = {
            .server_host = "10.0.0.1", .user = "u", .password = "p",
            .local_addr = { FX_LOCAL_IP, 5060 }, .rtp_port = 4000,
            .send = bad_send, .rand = bad_rand, .n_codecs = n_bad[i],
        };
        memcpy(c.codecs, bad[i], sizeof(c.codecs));
        sip_ua_t *ua = NULL;
        TEST_ASSERT_EQUAL(SIP_ERR_ARG, sip_ua_create(&c, &ua));
    }
}

static void test_incoming_no_codec_488(void)
{
    fx_init(f, NULL);
    inject_invite("in5@x", "z9hG4bKinv5", 1, FX_SDP_G729_ONLY, NULL);
    TEST_ASSERT_TRUE(fx_find(f, "SIP/2.0 488", 0) >= 0);
    TEST_ASSERT_EQUAL(0, fx_count_ev(f, FXE_INCOMING));
    TEST_ASSERT_EQUAL(0, sip_ua_debug_call_slots(f->ua));
}

static void test_incoming_remote_bye_and_cseq(void)
{
    fx_init(f, NULL);
    const char *t;
    sip_call_handle_t h = establish_incoming("in6@x", &t);
    char tag[80];
    snprintf(tag, sizeof(tag), "%s", t);
    /* stale CSeq → 500 (§12.2.2) */
    inject_from_bob("INFO", 99, "in6@x", tag, "z9hG4bKinfo0", NULL);
    TEST_ASSERT_TRUE(fx_find(f, "SIP/2.0 500", 0) >= 0);
    inject_from_bob("INFO", 101, "in6@x", tag, "z9hG4bKinfo1", NULL);
    int r = fx_find(f, "SIP/2.0 200", 0);
    TEST_ASSERT_EQUAL(SIP_METHOD_INFO, fx_parse(f, r)->cseq_method);
    inject_from_bob("UPDATE", 102, "in6@x", tag, "z9hG4bKupd", NULL);
    TEST_ASSERT_TRUE(fx_find(f, "SIP/2.0 501", 0) >= 0);
    inject_from_bob("OPTIONS", 103, "in6@x", tag, "z9hG4bKopt", NULL);
    r = fx_find(f, "SIP/2.0 200", 0);
    TEST_ASSERT_EQUAL(SIP_METHOD_OPTIONS, fx_parse(f, r)->cseq_method);
    TEST_ASSERT_NOT_NULL(fx_hdr(f, r, "Allow"));
    inject_from_bob("BYE", 104, "in6@x", tag, "z9hG4bKbye", NULL);
    r = fx_find(f, "SIP/2.0 200", 0);
    TEST_ASSERT_EQUAL(SIP_METHOD_BYE, fx_parse(f, r)->cseq_method);
    TEST_ASSERT_EQUAL(SIP_END_REMOTE, last_call_ev()->reason);
    TEST_ASSERT_EQUAL(SIP_UA_CALL_IDLE, sip_call_state(f->ua, h));
    /* BYE for a dialog that no longer exists → 481 */
    inject_from_bob("BYE", 105, "in6@x", tag, "z9hG4bKbye2", NULL);
    TEST_ASSERT_TRUE(fx_find(f, "SIP/2.0 481", 0) >= 0);
}

static void test_remote_reinvite_hold(void)
{
    fx_init(f, NULL);
    const char *t;
    establish_incoming("in7@x", &t);
    char tag[80];
    snprintf(tag, sizeof(tag), "%s", t);
    inject_from_bob("INVITE", 101, "in7@x", tag, "z9hG4bKre1", FX_SDP_SENDONLY);
    int ok = fx_find(f, "SIP/2.0 200", 0);
    TEST_ASSERT_EQUAL(SIP_METHOD_INVITE, fx_parse(f, ok)->cseq_method);
    TEST_ASSERT_NOT_NULL(strstr(f->sent[ok].buf, "a=recvonly\r\n"));
    TEST_ASSERT_NOT_NULL(strstr(f->sent[ok].buf, "o=sipwire "));
    TEST_ASSERT_EQUAL(SIP_DIR_RECVONLY, fx_last_ev(f, FXE_MEDIA)->media.direction);
    TEST_ASSERT_EQUAL(SIP_UA_CALL_ACTIVE, last_call_ev()->state);
    inject_from_bob("ACK", 101, "in7@x", tag, "z9hG4bKre1ack", NULL);
    fx_advance(f, 2000);
    TEST_ASSERT_EQUAL(1, fx_count(f, "SIP/2.0 200 OK\r\nVia: SIP/2.0/UDP 10.0.0.1:5060;rport=5060;branch=z9hG4bKre1"));
    /* unhold */
    inject_from_bob("INVITE", 102, "in7@x", tag, "z9hG4bKre2", FX_SDP_PCMU);
    ok = fx_find(f, "SIP/2.0 200", 0);
    TEST_ASSERT_NOT_NULL(strstr(f->sent[ok].buf, "a=sendrecv\r\n"));
    TEST_ASSERT_EQUAL(SIP_DIR_SENDRECV, fx_last_ev(f, FXE_MEDIA)->media.direction);
    inject_from_bob("ACK", 102, "in7@x", tag, "z9hG4bKre2ack", NULL);
    /* re-INVITE with unusable SDP → 488, call continues */
    inject_from_bob("INVITE", 103, "in7@x", tag, "z9hG4bKre3", FX_SDP_G729_ONLY);
    TEST_ASSERT_TRUE(fx_find(f, "SIP/2.0 488", 0) >= 0);
    TEST_ASSERT_EQUAL(SIP_UA_CALL_ACTIVE, sip_call_state(f->ua, last_call_ev()->call));
}

static void test_no_ack_for_2xx(void)
{
    fx_init(f, NULL);
    inject_invite("in8@x", "z9hG4bKinv8", 1, FX_SDP_PCMU, NULL);
    sip_call_answer(f->ua, fx_last_ev(f, FXE_INCOMING)->call);
    fx_advance(f, 32100);
    TEST_ASSERT_EQUAL(1, fx_count(f, "BYE"));
    TEST_ASSERT_EQUAL(SIP_END_TIMEOUT, last_call_ev()->reason);
}

static void test_hangup_before_ack(void)
{
    fx_init(f, NULL);
    inject_invite("in9@x", "z9hG4bKinv9", 1, FX_SDP_PCMU, NULL);
    sip_call_handle_t h = fx_last_ev(f, FXE_INCOMING)->call;
    sip_call_answer(f->ua, h);
    char tag[80];
    snprintf(tag, sizeof(tag), "%s", to_tag_of(fx_find(f, "SIP/2.0 200", 0)));
    sip_call_hangup(f->ua, h);
    TEST_ASSERT_EQUAL(0, fx_count(f, "BYE")); /* §15: wait for the ACK */
    TEST_ASSERT_EQUAL(SIP_UA_CALL_ENDED, last_call_ev()->state);
    inject_from_bob("ACK", 1, "in9@x", tag, "z9hG4bKack9", NULL);
    TEST_ASSERT_EQUAL(1, fx_count(f, "BYE"));
    TEST_ASSERT_EQUAL(0, sip_ua_debug_call_slots(f->ua));
}

static void test_delayed_offer_incoming(void)
{
    fx_init(f, NULL);
    inject_invite("in10@x", "z9hG4bKinv10", 1, NULL, NULL);
    sip_call_handle_t h = fx_last_ev(f, FXE_INCOMING)->call;
    sip_call_answer(f->ua, h);
    int ok = fx_find(f, "SIP/2.0 200", 0);
    TEST_ASSERT_NOT_NULL(strstr(f->sent[ok].buf, "m=audio 4000 RTP/AVP 9 0 8 101\r\n")); /* our offer */
    TEST_ASSERT_EQUAL(0, fx_count_ev(f, FXE_MEDIA));
    char tag[80];
    snprintf(tag, sizeof(tag), "%s", to_tag_of(ok));
    inject_from_bob("ACK", 1, "in10@x", tag, "z9hG4bKack10", FX_SDP_PCMA_ONLY);
    TEST_ASSERT_EQUAL(1, fx_count_ev(f, FXE_MEDIA));
    TEST_ASSERT_EQUAL(8, fx_last_ev(f, FXE_MEDIA)->media.payload_type);
}

static void test_out_of_dialog_requests(void)
{
    fx_init(f, NULL);
    inject_fmt("OPTIONS sip:1001@10.0.0.2:5060 SIP/2.0\r\n"
               "Via: SIP/2.0/UDP 10.0.0.1:5060;rport;branch=z9hG4bKo1\r\n"
               "From: <sip:asterisk@10.0.0.1>;tag=q1\r\nTo: <sip:1001@10.0.0.2>\r\n"
               "Call-ID: q1@x\r\nCSeq: 1 OPTIONS\r\nMax-Forwards: 70\r\nContent-Length: 0\r\n\r\n");
    int r = fx_find(f, "SIP/2.0 200", 0);
    TEST_ASSERT_TRUE(r >= 0);
    TEST_ASSERT_EQUAL_STRING("INVITE, ACK, BYE, CANCEL, OPTIONS, INFO, NOTIFY", fx_hdr(f, r, "Allow"));
    TEST_ASSERT_TRUE(fx_parse(f, r)->to.tag.len > 0);
    TEST_ASSERT_EQUAL_HEX32(FX_SERVER_IP, f->sent[r].to.ip);
    inject_fmt("FOO sip:1001@10.0.0.2 SIP/2.0\r\nVia: SIP/2.0/UDP 10.0.0.1;branch=z9hG4bKf\r\n"
               "From: <sip:a@x>;tag=1\r\nTo: <sip:1001@x>\r\nCall-ID: f@x\r\nCSeq: 1 FOO\r\n\r\n");
    TEST_ASSERT_TRUE(fx_find(f, "SIP/2.0 501", 0) >= 0);
    inject_fmt("SUBSCRIBE sip:1001@10.0.0.2 SIP/2.0\r\nVia: SIP/2.0/UDP 10.0.0.1;branch=z9hG4bKs\r\n"
               "From: <sip:a@x>;tag=1\r\nTo: <sip:1001@x>\r\nCall-ID: s@x\r\nCSeq: 1 SUBSCRIBE\r\n\r\n");
    TEST_ASSERT_TRUE(fx_find(f, "SIP/2.0 405", 0) >= 0);
    inject_fmt("INVITE sip:1001@10.0.0.2 SIP/2.0\r\nVia: SIP/2.0/UDP 10.0.0.1;branch=z9hG4bKr\r\n"
               "From: <sip:a@x>;tag=1\r\nTo: <sip:1001@x>\r\nCall-ID: r@x\r\nCSeq: 1 INVITE\r\n"
               "Require: 100rel\r\nContent-Length: 0\r\n\r\n");
    r = fx_find(f, "SIP/2.0 420", 0);
    TEST_ASSERT_TRUE(r >= 0);
    TEST_ASSERT_EQUAL_STRING("100rel", fx_hdr(f, r, "Unsupported"));
    inject_fmt("OPTIONS tel:+123 SIP/2.0\r\nVia: SIP/2.0/UDP 10.0.0.1;branch=z9hG4bKt\r\n"
               "From: <sip:a@x>;tag=1\r\nTo: <sip:1001@x>\r\nCall-ID: t@x\r\nCSeq: 1 OPTIONS\r\n\r\n");
    TEST_ASSERT_TRUE(fx_find(f, "SIP/2.0 416", 0) >= 0);
    /* datagrams from elsewhere are ignored */
    int n0 = f->n_sent;
    sip_addr_t rogue = { 0x0a000063, 5060 };
    const char *inv = "INVITE sip:1001@10.0.0.2 SIP/2.0\r\nVia: SIP/2.0/UDP 10.0.0.99;branch=z9hG4bKx\r\n"
                      "From: <sip:a@x>;tag=1\r\nTo: <sip:1001@x>\r\nCall-ID: x@x\r\nCSeq: 1 INVITE\r\n\r\n";
    sip_ua_on_packet(f->ua, inv, strlen(inv), &rogue);
    TEST_ASSERT_EQUAL(n0, f->n_sent);
    /* garbage is dropped */
    fx_inject(f, "garbage\r\n\r\n");
    TEST_ASSERT_EQUAL(n0, f->n_sent);
}

static void test_callback_reentrancy(void)
{
    fx_init(f, NULL);
    f->auto_answer = true;      /* answer from inside incoming_call */
    f->hangup_in_callback = true; /* and hang up from inside call_state(ACTIVE) */
    inject_invite("re1@x", "z9hG4bKre1", 1, FX_SDP_PCMU, NULL);
    TEST_ASSERT_EQUAL(1, fx_count(f, "SIP/2.0 200"));
    const fx_ev_t *e = last_call_ev();
    TEST_ASSERT_EQUAL(SIP_UA_CALL_ENDED, e->state);
    /* order: RINGING_IN, ACTIVE, ENDED */
    int seen = 0;
    for (int i = 0; i < f->n_ev; i++) {
        if (f->ev[i].kind == FXE_CALL) {
            static const int exp[] = { SIP_UA_CALL_RINGING_IN, SIP_UA_CALL_ACTIVE, SIP_UA_CALL_ENDED };
            TEST_ASSERT_TRUE(seen < 3);
            TEST_ASSERT_EQUAL(exp[seen++], f->ev[i].state);
        }
    }
    TEST_ASSERT_EQUAL(3, seen);
    /* destroy from inside a callback is deferred safely (ASan checks) */
    f->auto_answer = false;
    f->hangup_in_callback = false;
    f->destroy_in_callback = true;
    inject_invite("re2@x", "z9hG4bKre2", 1, FX_SDP_PCMU, NULL);
    sip_call_handle_t h = fx_last_ev(f, FXE_INCOMING)->call;
    sip_call_reject(f->ua, h, 486); /* ENDED → callback destroys the UA */
    TEST_ASSERT_NULL(f->ua);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_register_401_then_200);
    RUN_TEST(test_register_refresh_timing);
    RUN_TEST(test_register_wrong_password);
    RUN_TEST(test_register_failure_backoff);
    RUN_TEST(test_register_timer_f);
    RUN_TEST(test_register_423);
    RUN_TEST(test_register_nat_rport);
    RUN_TEST(test_unregister);
    RUN_TEST(test_keepalive_crlf);
    RUN_TEST(test_keepalive_options_timeout_reregisters);
    RUN_TEST(test_outgoing_call_success);
    RUN_TEST(test_outgoing_with_proxy_auth);
    RUN_TEST(test_outgoing_busy);
    RUN_TEST(test_txn_pool_burst);
    RUN_TEST(test_outgoing_timer_b);
    RUN_TEST(test_cancel_early);
    RUN_TEST(test_cancel_before_provisional);
    RUN_TEST(test_cancel_200_race);
    RUN_TEST(test_outgoing_no_common_codec);
    RUN_TEST(test_remote_bye_outgoing);
    RUN_TEST(test_local_hold_resume);
    RUN_TEST(test_dial_rules);
    RUN_TEST(test_incoming_answer);
    RUN_TEST(test_incoming_retransmitted_invite);
    RUN_TEST(test_incoming_reject);
    RUN_TEST(test_incoming_reject_no_ack_timer_h);
    RUN_TEST(test_incoming_remote_cancel);
    RUN_TEST(test_incoming_busy_second_call);
    RUN_TEST(test_outgoing_g722_answer);
    RUN_TEST(test_incoming_g722_offer_answered_in_pref_order);
    RUN_TEST(test_codec_preference_config);
    RUN_TEST(test_incoming_no_codec_488);
    RUN_TEST(test_incoming_remote_bye_and_cseq);
    RUN_TEST(test_remote_reinvite_hold);
    RUN_TEST(test_no_ack_for_2xx);
    RUN_TEST(test_hangup_before_ack);
    RUN_TEST(test_delayed_offer_incoming);
    RUN_TEST(test_out_of_dialog_requests);
    RUN_TEST(test_callback_reentrancy);
    return UNITY_END();
}
