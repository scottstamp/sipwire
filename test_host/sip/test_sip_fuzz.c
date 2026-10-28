/* Deterministic mutation fuzzing of everything that touches network input:
 * the message parser, URI / name-addr / digest / SDP sub-parsers and the
 * full UA (fed through sip_ua_on_packet). Each mutated datagram lives in an
 * exactly-sized heap block so ASan flags any out-of-bounds read. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sip_auth.h"
#include "sip_dialog.h"
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

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_fuzz_parser);
    RUN_TEST(test_fuzz_ua);
    return UNITY_END();
}
