/* Deterministic mutation fuzzing of everything that touches network input:
 * the message parser, URI / name-addr / digest / SDP sub-parsers, the event
 * package parsers (dialog-info XML, message-summary, Subscription-State) and
 * the full UA (fed through sip_ua_on_packet, incl. NOTIFYs in live
 * subscription dialogs). Each mutated datagram lives in an
 * exactly-sized heap block so ASan flags any out-of-bounds read. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sip_auth.h"
#include "sip_dialog.h"
#include "sip_event.h"
#include "sip_msg.h"
#include "sip_sdp.h"
#include "sip_test_util.h"
#include "sip_ua_internal.h"
#include "unity.h"

#ifndef FUZZ_ITERATIONS
#define FUZZ_ITERATIONS 50000
#endif

static const char *seeds[] = {
    "INVITE sip:1001@10.0.0.2:5060 SIP/2.0\r\n"
    "Via: SIP/2.0/UDP 10.0.0.1:5060;rport;branch=z9hG4bKPj1\r\n"
    "Via: SIP/2.0/UDP 10.0.0.9:5070;branch=z9hG4bKPj0, SIP/2.0/UDP [2001:db8::1];branch=z9hG4bKx\r\n"
    "Max-Forwards: 70\r\n"
    "From: \"Bob \\\"B\\\"\" <sip:1002@10.0.0.1>;tag=remote1\r\n"
    "To: <sip:1001@10.0.0.2>\r\n"
    "Contact: <sip:asterisk@10.0.0.1:5060>;expires=60\r\n"
    "Record-Route: <sip:10.0.0.1;lr>, <sip:p2.example.com;lr;transport=udp>\r\n"
    "Call-ID: fuzz1@10.0.0.1\r\n"
    "CSeq: 100 INVITE\r\n"
    "Supported: timer, replaces\r\n"
    "Alert-Info: <http://x/r.wav>;info=find, find;x=1, info=intercom\r\n"
    "Call-Info: <sip:10.0.0.1>;answer-after=0\r\n"
    "Content-Type: application/sdp\r\n"
    "\r\n" /* no Content-Length: rest of datagram (§18.3) */
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
    "a=sendrecv\r\n"
    "m=video 0 RTP/AVP 31\r\n",

    "SIP/2.0 401 Unauthorized\r\n"
    "Via: SIP/2.0/UDP 10.0.0.2:5060;rport=5060;received=10.0.0.2;branch=z9hG4bK0001\r\n"
    "From: \"Alice\" <sip:1001@10.0.0.1>;tag=abcd\r\n"
    "To: \"Alice\" <sip:1001@10.0.0.1>;tag=srv\r\n"
    "Call-ID: reg@10.0.0.2\r\n"
    "CSeq: 1 REGISTER\r\n"
    "WWW-Authenticate: Digest realm=\"asterisk\",nonce=\"16\\\"9:x\",opaque=\"o\",algorithm=MD5,qop=\"auth\"\r\n"
    "Content-Length: 0\r\n\r\n",

    "SIP/2.0 200 OK\r\n"
    "v: SIP/2.0/UDP 10.0.0.2:5060;rport=40000;received=203.0.113.5;branch=z9hG4bK0002\r\n"
    "f: <sip:1001@10.0.0.1>;tag=abcd\r\n"
    "t: <sip:600@10.0.0.1>;tag=callee\r\n"
    "i: call@10.0.0.2\r\n"
    "CSeq: 1 INVITE\r\n"
    "m: <sip:srv@10.0.0.1:5060>, *\r\n"
    "Expires: 300\r\n"
    "c: application/sdp\r\n"
    "\r\n"
    "v=0\r\n"
    "c=IN IP4 10.0.0.1\r\n"
    "m=audio 20000 RTP/AVP 8\r\n"
    "a=rtpmap:8 PCMA/8000\r\n"
    "a=sendonly\r\n"
    "a=ptime:30\r\n",

    "BYE sip:1001@10.0.0.2:5060 SIP/2.0\r\n"
    "Via: SIP/2.0/UDP 10.0.0.1:5060;rport;branch=z9hG4bKbye\r\n"
    "From: <sip:600@10.0.0.1>;tag=callee\r\n"
    "To: \"Alice\" <sip:1001@10.0.0.1>;tag=abcd\r\n"
    "Route: <sip:%61b@10.0.0.1;lr>\r\n"
    "Call-ID: call@10.0.0.2\r\n"
    "CSeq: 2 BYE\r\n"
    "Content-Length: 0\r\n\r\n",

    "CANCEL sip:1001@10.0.0.2:5060 SIP/2.0\r\n"
    "Via: SIP/2.0/UDP 10.0.0.1:5060;rport;branch=z9hG4bKPj1\r\n"
    "From: \"Bob\" <sip:1002@10.0.0.1>;tag=remote1\r\n"
    "To: <sip:1001@10.0.0.2>\r\n"
    "Call-ID: fuzz1@10.0.0.1\r\n"
    "CSeq: 100 CANCEL\r\n"
    "Content-Length: 0\r\n\r\n",
};

/* event package bodies (fed to their parsers directly) */
static const char *body_seeds[] = {
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n"
    "<dialog-info xmlns=\"urn:ietf:params:xml:ns:dialog-info\" version=\"3\" state=\"full\" "
    "entity=\"sip:1002@127.0.0.1:5080\">\r\n"
    " <dialog id=\"1002\" direction=\"recipient\">\r\n"
    "  <state>early</state>\r\n"
    "  <local><target uri=\"sip:1002@x\"><param pname=\"+sip.rendering\" pvalue=\"no\"/></target></local>\r\n"
    " </dialog>\r\n"
    " <!-- c --><dialog id='b'><state>confirmed</state></dialog><dialog id=\"c\"/>\r\n"
    "</dialog-info>\r\n",

    "<d:dialog-info xmlns:d=\"urn:ietf:params:xml:ns:dialog-info\" version=\"9\" state=\"partial\">"
    "<d:dialog id=\"1\"><d:state>terminated</d:state></d:dialog></d:dialog-info>",

    "Messages-Waiting: yes\r\nMessage-Account: sip:1001@127.0.0.1\r\nVoice-Message: 2/8 (0/2)\r\n"
    "Fax-Message: 1/1\r\n\r\nTo: <a@b>\r\n",
};

static const char *hdr_seeds[] = {
    "active;expires=600",
    "terminated;reason=noresource;retry-after=30",
    "message-summary;id=12",
};

static const char interesting[] = ":;,\"<>\\\r\n \t%@=?/[]0z*\x7f\x80\xff";

void setUp(void) {}
void tearDown(void) {}

static uint32_t rng = 0xC0FFEE11;

static size_t mutate(char *dst, size_t cap, const char *src, size_t len)
{
    if (len > cap) {
        len = cap;
    }
    memcpy(dst, src, len);
    int ops = 1 + (int)(fx_rand(&rng) % 6);
    for (int k = 0; k < ops; k++) {
        uint32_t r = fx_rand(&rng);
        size_t pos = len ? fx_rand(&rng) % len : 0;
        switch (r % 7) {
        case 0: /* flip a byte */
            if (len) {
                dst[pos] = (char)(fx_rand(&rng) & 0xff);
            }
            break;
        case 1: /* interesting byte */
            if (len) {
                dst[pos] = interesting[fx_rand(&rng) % (sizeof(interesting) - 1)];
            }
            break;
        case 2: /* insert */
            if (len < cap) {
                memmove(dst + pos + 1, dst + pos, len - pos);
                dst[pos] = interesting[fx_rand(&rng) % (sizeof(interesting) - 1)];
                len++;
            }
            break;
        case 3: { /* delete a run */
            size_t n = 1 + fx_rand(&rng) % 16;
            if (pos + n > len) {
                n = len - pos;
            }
            memmove(dst + pos, dst + pos + n, len - pos - n);
            len -= n;
            break;
        }
        case 4: /* truncate */
            len = pos;
            break;
        case 5: { /* duplicate a chunk */
            size_t n = 1 + fx_rand(&rng) % 64;
            if (pos + n > len) {
                n = len - pos;
            }
            if (len + n <= cap) {
                memmove(dst + pos + n, dst + pos, len - pos);
                len += n;
            }
            break;
        }
        default: { /* overwrite digits with huge numbers */
            const char *big = "99999999999";
            size_t n = strlen(big);
            if (pos + n <= len) {
                memcpy(dst + pos, big, n);
            }
            break;
        }
        }
    }
    return len;
}

static void exercise_subparsers(const sip_msg_t *m)
{
    sip_uri_t u;
    char tmp[256];
    sip_uri_parse(m->ruri, &u);
    sip_uri_parse(m->from.uri, &u);
    sip_uri_parse(m->to.uri, &u);
    if (u.user.len) {
        sip_unescape(tmp, sizeof(tmp), u.user);
    }
    sip_nameaddr_format(tmp, sizeof(tmp), &m->from);
    for (int i = 0; i < m->n_contact; i++) {
        sip_uri_parse(m->contact[i].uri, &u);
    }
    for (int i = 0; i < m->n_rr; i++) {
        sip_nameaddr_t na;
        sip_nameaddr_parse(m->rr[i].raw, &na, false);
    }
    sip_auth_t a;
    for (int i = 0; i < m->n_www_auth; i++) {
        sip_auth_parse_challenge(m->www_auth[i], false, &a);
    }
    sip_sdp_t s;
    if (sip_sdp_parse(&s, m->body.p, m->body.len) == 0) {
        static const uint8_t prefs[] = { 0, 8 };
        sip_sdp_neg_t n;
        sip_sdp_negotiate(&s, prefs, 2, true, true, &n);
        sip_sdp_negotiate(&s, prefs, 2, true, false, &n);
    }
    sip_dialog_t d;
    if (m->is_request) {
        sip_dialog_init_uas(&d, m, "tag");
    } else {
        sip_dialog_init_uac(&d, m, "cid", "<sip:a@b>", "t", "<sip:c@d>", "sip:c@d", 1);
    }
}

static void test_fuzz_parser(void)
{
    static char work[SIP_MAX_MSG_IN + 256];
    int ok = 0;
    for (int it = 0; it < FUZZ_ITERATIONS; it++) {
        const char *seed = seeds[fx_rand(&rng) % (sizeof(seeds) / sizeof(seeds[0]))];
        size_t len = mutate(work, sizeof(work), seed, strlen(seed));
        char *exact = malloc(len ? len : 1);
        TEST_ASSERT_NOT_NULL(exact);
        memcpy(exact, work, len);
        sip_msg_t m;
        if (sip_msg_parse(&m, exact, len) == SIP_PARSE_OK) {
            ok++;
            exercise_subparsers(&m);
        }
        /* SDP parser directly on mutated bodies too */
        sip_sdp_t s;
        sip_sdp_parse(&s, exact, len);
        free(exact);

        /* event bodies and headers */
        const char *bs = body_seeds[fx_rand(&rng) % (sizeof(body_seeds) / sizeof(body_seeds[0]))];
        len = mutate(work, sizeof(work), bs, strlen(bs));
        exact = malloc(len ? len : 1);
        TEST_ASSERT_NOT_NULL(exact);
        memcpy(exact, work, len);
        sip_dlginfo_t di;
        sip_msgsum_t ms;
        sip_dlginfo_parse(exact, len, &di);
        sip_msgsum_parse(exact, len, &ms);
        free(exact);
        const char *hs = hdr_seeds[fx_rand(&rng) % (sizeof(hdr_seeds) / sizeof(hdr_seeds[0]))];
        len = mutate(work, sizeof(work), hs, strlen(hs));
        exact = malloc(len ? len : 1);
        TEST_ASSERT_NOT_NULL(exact);
        memcpy(exact, work, len);
        sip_sub_state_t ss;
        sip_str_t pkg, id;
        sip_sub_state_parse((sip_str_t){ exact, len }, &ss);
        sip_event_parse((sip_str_t){ exact, len }, &pkg, &id);
        free(exact);
    }
    printf("fuzz: %d/%d mutated messages still parsed\n", ok, FUZZ_ITERATIONS);
    TEST_ASSERT_TRUE(ok > 0);
}

static void quiet_cfg(sip_ua_config_t *c)
{
    c->log_level = SIP_LOG_ERROR;
}

static void test_fuzz_ua(void)
{
    static fx_t f;
    static char work[SIP_MAX_MSG_IN + 256];
    fx_init(&f, quiet_cfg);
    sip_ua_register(f.ua);
    TEST_ASSERT_EQUAL(SIP_OK, sip_ua_set_alert_marker(f.ua, "find"));
    static const char *const targets[] = { "1003", "sip:vm@10.0.0.1", "", "#*+1 (555)", "sip:bad>uri" };
    for (int it = 0; it < FUZZ_ITERATIONS / 5; it++) {
        /* screening churn: default, ring, reject, redirect (incl. unusable targets) */
        f.screen_on = it % 3 != 0;
        f.screen.action = (sip_screen_action_t)(fx_rand(&rng) % 4);
        f.screen.code = (int)(fx_rand(&rng) % 800);
        snprintf(f.screen.target, sizeof(f.screen.target), "%s", targets[fx_rand(&rng) % 5]);
        const char *seed = seeds[fx_rand(&rng) % (sizeof(seeds) / sizeof(seeds[0]))];
        size_t len = mutate(work, sizeof(work), seed, strlen(seed));
        char *exact = malloc(len ? len : 1);
        TEST_ASSERT_NOT_NULL(exact);
        memcpy(exact, work, len);
        sip_ua_on_packet(f.ua, exact, len, &f.server);
        free(exact);
        if (it % 7 == 0) {
            fx_advance(&f, 50 + fx_rand(&rng) % 2000);
        }
        if (it % 97 == 0) {
            sip_call_handle_t h;
            sip_call_dial(f.ua, "600", &h);
        }
        if (it % 83 == 0 && f.n_ev) {
            sip_call_redirect(f.ua, f.ev[f.n_ev - 1].call, targets[fx_rand(&rng) % 5]);
        }
        if (it % 89 == 0 && f.n_ev) {
            sip_call_answer(f.ua, f.ev[f.n_ev - 1].call);
            sip_call_hangup(f.ua, f.ev[f.n_ev - 1].call);
        }
        f.n_sent = 0;
        f.n_ev = 0;
    }
    fx_advance(&f, 120000);
    fx_free(&f);
}

/* NOTIFY for the subscription whose SUBSCRIBE is f->sent[idx] */
static void notify_seed(fx_t *f, int idx, const char *event, const char *ctype, const char *body, char *out,
                        size_t cap, const char *contact)
{
    const sip_msg_t *m = fx_parse(f, idx);
    snprintf(out, cap,
             "NOTIFY sip:1001@10.0.0.2:5060 SIP/2.0\r\n"
             "Via: SIP/2.0/UDP 10.0.0.1:5060;rport;branch=z9hG4bKnf1\r\n"
             "Max-Forwards: 70\r\n"
             "From: <sip:1002@10.0.0.1>;tag=srv\r\n"
             "To: <sip:1001@10.0.0.1>;tag=%.*s\r\n"
             "Contact: <%s>\r\n"
             "Call-ID: %.*s\r\n"
             "CSeq: 7 NOTIFY\r\n"
             "Event: %s\r\n"
             "Subscription-State: active;expires=600\r\n"
             "Content-Type: %s\r\n"
             "Content-Length: %u\r\n\r\n%s",
             (int)m->from.tag.len, m->from.tag.p, contact, (int)m->call_id.len, m->call_id.p, event, ctype,
             (unsigned)strlen(body), body);
}

/* NOTIFYs (and their mutations) into live BLF / MWI subscriptions, plus
 * unsolicited MWI, with subscribe / unsubscribe churn in between. */
static void test_fuzz_notify(void)
{
    static fx_t f;
    static char work[SIP_MAX_MSG_IN + 256];
    static char seed[5][2048];
    /* a target refresh whose Contact URI exceeds SIP_URI_MAX (must keep the old target) */
    static char long_contact[256];
    snprintf(long_contact, sizeof(long_contact), "sip:1002@10.0.0.1:5060;x=%0200d", 0);
    int n_blf = 0, n_mwi = 0;
    fx_init(&f, quiet_cfg);
    sip_ua_register(f.ua);
    fx_reply(&f, 0, 200, "s1", "Expires: 300\r\n", NULL);
    f.auto_reg = true;
    for (int it = 0; it < FUZZ_ITERATIONS / 5; it++) {
        if (it % 500 == 0) {
            /* (re)start subscriptions and rebuild seeds for their dialogs */
            fx_clear_sent(&f);
            sip_ua_unregister(f.ua);
            fx_advance(&f, 33000); /* un-SUBSCRIBEs (never answered) time out */
            sip_ua_register(f.ua);
            fx_advance(&f, 20);
            TEST_ASSERT_EQUAL(SIP_UA_REG_REGISTERED, sip_ua_reg_state(f.ua));
            fx_clear_sent(&f);
            sip_sub_handle_t h;
            TEST_ASSERT_EQUAL(SIP_OK, sip_ua_subscribe_blf(f.ua, "1002", &h));
            TEST_ASSERT_EQUAL(SIP_OK, sip_ua_subscribe_mwi(f.ua, &h));
            int b = -1, w = -1;
            for (int i = 0; i < f.n_sent; i++) {
                if (strncmp(f.sent[i].buf, "SUBSCRIBE", 9) == 0) {
                    if (strstr(f.sent[i].buf, "Event: dialog")) {
                        b = i;
                    } else {
                        w = i;
                    }
                }
            }
            TEST_ASSERT_TRUE(b >= 0 && w >= 0);
            const char *ct = "sip:1002@10.0.0.1:5060";
            notify_seed(&f, b, "dialog", "application/dialog-info+xml", body_seeds[0], seed[0], sizeof(seed[0]), ct);
            notify_seed(&f, b, "dialog", "application/dialog-info+xml", body_seeds[1], seed[1], sizeof(seed[1]), ct);
            notify_seed(&f, w, "message-summary", "application/simple-message-summary", body_seeds[2], seed[2],
                        sizeof(seed[2]), ct);
            notify_seed(&f, b, "dialog", "application/dialog-info+xml", body_seeds[0], seed[4], sizeof(seed[4]),
                        long_contact);
            snprintf(seed[3], sizeof(seed[3]),
                     "NOTIFY sip:1001@10.0.0.2:5060 SIP/2.0\r\n"
                     "Via: SIP/2.0/UDP 10.0.0.1:5060;rport;branch=z9hG4bKnf2\r\n"
                     "From: <sip:1001@10.0.0.1>;tag=u\r\nTo: <sip:1001@10.0.0.2:5060>\r\n"
                     "Call-ID: mwi@x\r\nCSeq: 1 NOTIFY\r\nEvent: message-summary\r\n"
                     "Content-Type: application/simple-message-summary\r\n"
                     "Content-Length: %u\r\n\r\n%s",
                     (unsigned)strlen(body_seeds[2]), body_seeds[2]);
            fx_reply(&f, b, 200, "srv", "Expires: 600\r\n", NULL);
            fx_reply(&f, w, 200, "srv", "Expires: 600\r\n", NULL);
        }
        const char *sd = seed[fx_rand(&rng) % 5];
        size_t len = it % 10 == 0 ? strlen(sd) : mutate(work, sizeof(work), sd, strlen(sd));
        char *exact = malloc(len ? len : 1);
        TEST_ASSERT_NOT_NULL(exact);
        memcpy(exact, it % 10 == 0 ? sd : work, len);
        sip_ua_on_packet(f.ua, exact, len, &f.server);
        free(exact);
        if (f.n_sent > FX_MAX_SENT / 2) {
            fx_clear_sent(&f);
        }
        if (it % 13 == 0) {
            fx_advance(&f, 50 + fx_rand(&rng) % 5000);
        }
        n_blf += fx_count_ev(&f, FXE_BLF);
        n_mwi += fx_count_ev(&f, FXE_MWI);
        f.n_ev = 0;
    }
    printf("fuzz: %d BLF and %d MWI reports from mutated NOTIFYs\n", n_blf, n_mwi);
    TEST_ASSERT_TRUE(n_blf > 10 && n_mwi > 10);
    fx_clear_sent(&f);
    fx_advance(&f, 60000);
    fx_free(&f);
}

static char *find_mem(char *p, size_t len, const char *needle)
{
    size_t n = strlen(needle);
    for (size_t i = 0; i + n <= len; i++) {
        if (memcmp(p + i, needle, n) == 0) {
            return p + i;
        }
    }
    return NULL;
}

/* snprintf into a char array that must not truncate */
#define SEEDF(buf, ...) TEST_ASSERT_TRUE(snprintf(buf, sizeof(buf), __VA_ARGS__) < (int)sizeof(buf))

/* Header blocks sized to the UA's limits: n Via lines of about len bytes. */
static int big_vias(char *out, size_t cap, int n, int len, const char *branch)
{
    size_t o = 0;
    for (int i = 0; i < n && o + (size_t)len + 64 < cap; i++) {
        int k = snprintf(out + o, cap - o, "Via: SIP/2.0/UDP 10.0.0.%d:5060;rport;branch=%s%d;x=", i + 1, branch, i);
        o += (size_t)k;
        for (; k < len - 2; k++) {
            out[o++] = 'v';
        }
        out[o++] = '\r';
        out[o++] = '\n';
    }
    out[o] = '\0';
    return (int)o;
}

/* Seeds that reach the size limits of the response template (Via block over
 * 800 B; To / Record-Route long enough that the 180 or the 200 + SDP do not
 * fit 1500 B) and of the dialog (Contact URI over SIP_URI_MAX in a target
 * refresh), mutated around those edges. Afterwards nothing may be left
 * holding the transaction pool. */
static void test_fuzz_oversized(void)
{
    static fx_t f;
    static char work[SIP_MAX_MSG_IN + 256];
    static char seed[5][SIP_MAX_MSG_IN];
    static char vias[1400];
    fx_init(&f, quiet_cfg);
    sip_ua_register(f.ua);
    fx_reply(&f, 0, 200, "s1", "Expires: 300\r\n", NULL);
    f.auto_reg = true;

    /* a call to refresh the target of: answered, ACKed */
    f.auto_answer = true;
    const char *sdp = seeds[0] + (strstr(seeds[0], "\r\n\r\n") - seeds[0]) + 4;
    SEEDF(work,
             "INVITE sip:1001@10.0.0.2:5060 SIP/2.0\r\n"
             "Via: SIP/2.0/UDP 10.0.0.1:5060;rport;branch=z9hG4bKov0\r\n"
             "From: \"Bob\" <sip:1002@10.0.0.1>;tag=remote1\r\nTo: <sip:1001@10.0.0.2>\r\n"
             "Contact: <sip:asterisk@10.0.0.1:5060>\r\nCall-ID: ov@x\r\nCSeq: 1 INVITE\r\n"
             "Content-Type: application/sdp\r\nContent-Length: %u\r\n\r\n%s",
             (unsigned)strlen(sdp), sdp);
    fx_inject(&f, work);
    f.auto_answer = false;
    int ok = fx_find(&f, "SIP/2.0 200", 0);
    TEST_ASSERT_TRUE(ok >= 0);
    const sip_msg_t *om = fx_parse(&f, ok);
    char tag[40];
    snprintf(tag, sizeof(tag), "%.*s", (int)om->to.tag.len, om->to.tag.p);
    SEEDF(work,
             "ACK sip:1001@10.0.0.2:5060 SIP/2.0\r\nVia: SIP/2.0/UDP 10.0.0.1:5060;branch=z9hG4bKov0a\r\n"
             "From: \"Bob\" <sip:1002@10.0.0.1>;tag=remote1\r\nTo: <sip:1001@10.0.0.2>;tag=%s\r\n"
             "Call-ID: ov@x\r\nCSeq: 1 ACK\r\nContent-Length: 0\r\n\r\n", tag);
    fx_inject(&f, work);

    /* 0: new INVITE, 8 Vias over the template */
    big_vias(vias, sizeof(vias), SIP_MAX_VIA, 120, "z9hG4bKov1-");
    SEEDF(seed[0],
             "INVITE sip:1001@10.0.0.2:5060 SIP/2.0\r\n%s"
             "From: <sip:1003@10.0.0.1>;tag=r3\r\nTo: <sip:1001@10.0.0.2>\r\n"
             "Contact: <sip:x@10.0.0.1>\r\nCall-ID: ov1@x\r\nCSeq: 1 INVITE\r\nContent-Length: 0\r\n\r\n", vias);
    /* 1: new INVITE whose 200 + SDP does not fit (long To, Record-Route) */
    big_vias(vias, sizeof(vias), 5, 120, "z9hG4bKov2-");
    SEEDF(seed[1],
             "INVITE sip:1001@10.0.0.2:5060 SIP/2.0\r\n%s"
             "From: <sip:1004@10.0.0.1>;tag=r4\r\nTo: <sip:1001@10.0.0.2;x=%0140d>\r\n"
             "Record-Route: <sip:p1.example.com;lr;x=%090d>, <sip:p2.example.com;lr;x=%090d>\r\n"
             "Record-Route: <sip:p3.example.com;lr;x=%090d>\r\n"
             "Contact: <sip:y@10.0.0.1>\r\nCall-ID: ov2@x\r\nCSeq: 1 INVITE\r\n"
             "Content-Type: application/sdp\r\nContent-Length: %u\r\n\r\n%s",
             vias, 0, 0, 0, 0, (unsigned)strlen(sdp), sdp);
    /* 2: in-dialog re-INVITE with a Contact URI over SIP_URI_MAX */
    SEEDF(seed[2],
             "INVITE sip:1001@10.0.0.2:5060 SIP/2.0\r\nVia: SIP/2.0/UDP 10.0.0.1:5060;rport;branch=z9hG4bKov3\r\n"
             "From: \"Bob\" <sip:1002@10.0.0.1>;tag=remote1\r\nTo: <sip:1001@10.0.0.2>;tag=%s\r\n"
             "Contact: <sip:moved@10.0.0.1:5060;x=%0200d>\r\nCall-ID: ov@x\r\nCSeq: 5 INVITE\r\n"
             "Content-Type: application/sdp\r\nContent-Length: %u\r\n\r\n%s",
             tag, 0, (unsigned)strlen(sdp), sdp);
    /* 3: in-dialog OPTIONS / 4: CANCEL, both behind 8 long Vias */
    big_vias(vias, sizeof(vias), SIP_MAX_VIA, 110, "z9hG4bKov4-");
    SEEDF(seed[3],
             "OPTIONS sip:1001@10.0.0.2:5060 SIP/2.0\r\n%s"
             "From: \"Bob\" <sip:1002@10.0.0.1>;tag=remote1\r\nTo: <sip:1001@10.0.0.2>;tag=%s\r\n"
             "Call-ID: ov@x\r\nCSeq: 6 OPTIONS\r\nContent-Length: 0\r\n\r\n", vias, tag);
    SEEDF(seed[4],
             "CANCEL sip:1001@10.0.0.2:5060 SIP/2.0\r\n%s"
             "From: <sip:1004@10.0.0.1>;tag=r4\r\nTo: <sip:1001@10.0.0.2>\r\n"
             "Call-ID: ov2@x\r\nCSeq: 1 CANCEL\r\nContent-Length: 0\r\n\r\n", vias);
    for (int i = 0; i < 5; i++) {
        TEST_ASSERT_TRUE(strlen(seed[i]) < SIP_MAX_MSG_IN);
    }

    int cseq = 10;
    for (int it = 0; it < FUZZ_ITERATIONS / 10; it++) {
        int k = (int)(fx_rand(&rng) % 5);
        size_t len = it % 4 == 0 ? strlen(seed[k]) : mutate(work, sizeof(work), seed[k], strlen(seed[k]));
        char *exact = malloc(len ? len : 1);
        TEST_ASSERT_NOT_NULL(exact);
        memcpy(exact, it % 4 == 0 ? seed[k] : work, len);
        /* fresh branch / CSeq so unmutated seeds are new requests, not retransmissions */
        char *br = find_mem(exact, len, "z9hG4bKov");
        if (br && br + 12 <= exact + len) {
            br[9] = (char)('a' + it % 26);
            br[10] = (char)('a' + (it / 26) % 26);
            br[11] = (char)('a' + (it / 676) % 26);
        }
        char *cs = find_mem(exact, len, "CSeq: 5 INVITE");
        if (cs) {
            cs[6] = (char)('0' + cseq++ % 10); /* mostly out of order: 500 */
        }
        sip_ua_on_packet(f.ua, exact, len, &f.server);
        free(exact);
        if (it % 17 == 0 && f.n_ev) {
            sip_call_answer(f.ua, f.ev[f.n_ev - 1].call);
        }
        if (it % 7 == 0) {
            fx_advance(&f, 50 + fx_rand(&rng) % 3000);
        }
        fx_clear_sent(&f);
        f.n_ev = 0;
    }
    /* everything times out (incl. 180 s server INVITE / timer C bounds) */
    fx_advance(&f, 200000);
    printf("fuzz: %d transactions left after the oversized run\n", sip_ua_debug_txn_count(f.ua));
    TEST_ASSERT_TRUE(sip_ua_debug_txn_count(f.ua) <= 2); /* REGISTER refresh at most */
    fx_clear_sent(&f);
    sip_call_handle_t h;
    TEST_ASSERT_EQUAL(SIP_OK, sip_call_dial(f.ua, "600", &h));
    TEST_ASSERT_EQUAL(1, fx_count(&f, "INVITE"));
    fx_free(&f);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_fuzz_parser);
    RUN_TEST(test_fuzz_ua);
    RUN_TEST(test_fuzz_notify);
    RUN_TEST(test_fuzz_oversized);
    return UNITY_END();
}
