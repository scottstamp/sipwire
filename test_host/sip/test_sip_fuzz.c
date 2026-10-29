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
    for (int it = 0; it < FUZZ_ITERATIONS / 5; it++) {
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
                        size_t cap)
{
    const sip_msg_t *m = fx_parse(f, idx);
    snprintf(out, cap,
             "NOTIFY sip:1001@10.0.0.2:5060 SIP/2.0\r\n"
             "Via: SIP/2.0/UDP 10.0.0.1:5060;rport;branch=z9hG4bKnf1\r\n"
             "Max-Forwards: 70\r\n"
             "From: <sip:1002@10.0.0.1>;tag=srv\r\n"
             "To: <sip:1001@10.0.0.1>;tag=%.*s\r\n"
             "Contact: <sip:1002@10.0.0.1:5060>\r\n"
             "Call-ID: %.*s\r\n"
             "CSeq: 7 NOTIFY\r\n"
             "Event: %s\r\n"
             "Subscription-State: active;expires=600\r\n"
             "Content-Type: %s\r\n"
             "Content-Length: %u\r\n\r\n%s",
             (int)m->from.tag.len, m->from.tag.p, (int)m->call_id.len, m->call_id.p, event, ctype,
             (unsigned)strlen(body), body);
}

/* NOTIFYs (and their mutations) into live BLF / MWI subscriptions, plus
 * unsolicited MWI, with subscribe / unsubscribe churn in between. */
static void test_fuzz_notify(void)
{
    static fx_t f;
    static char work[SIP_MAX_MSG_IN + 256];
    static char seed[4][2048];
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
            notify_seed(&f, b, "dialog", "application/dialog-info+xml", body_seeds[0], seed[0], sizeof(seed[0]));
            notify_seed(&f, b, "dialog", "application/dialog-info+xml", body_seeds[1], seed[1], sizeof(seed[1]));
            notify_seed(&f, w, "message-summary", "application/simple-message-summary", body_seeds[2], seed[2],
                        sizeof(seed[2]));
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
        const char *sd = seed[fx_rand(&rng) % 4];
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

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_fuzz_parser);
    RUN_TEST(test_fuzz_ua);
    RUN_TEST(test_fuzz_notify);
    return UNITY_END();
}
