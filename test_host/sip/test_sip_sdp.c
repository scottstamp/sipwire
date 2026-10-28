/* SDP parse / negotiate / build. */

#include <string.h>

#include "sip_sdp.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static sip_sdp_t s;
static const uint8_t prefs_ua[] = { 0, 8 };
static const uint8_t prefs_au[] = { 8, 0 };

static int parse(const char *body)
{
    return sip_sdp_parse(&s, body, strlen(body));
}

static void test_parse_typical(void)
{
    TEST_ASSERT_EQUAL(0, parse("v=0\r\n"
                               "o=- 123 456 IN IP4 192.168.1.10\r\n"
                               "s=Asterisk\r\n"
                               "c=IN IP4 192.168.1.10\r\n"
                               "t=0 0\r\n"
                               "m=audio 12010 RTP/AVP 0 8 101\r\n"
                               "a=rtpmap:0 PCMU/8000\r\n"
                               "a=rtpmap:8 PCMA/8000\r\n"
                               "a=rtpmap:101 telephone-event/8000\r\n"
                               "a=fmtp:101 0-16\r\n"
                               "a=ptime:20\r\n"
                               "a=maxptime:150\r\n"
                               "a=sendrecv\r\n"));
    TEST_ASSERT_EQUAL_STRING("456", s.o_version);
    TEST_ASSERT_EQUAL(0, s.audio);
    TEST_ASSERT_EQUAL_HEX32(0xc0a8010a, s.audio_ip);
    TEST_ASSERT_EQUAL(12010, s.audio_port);
    TEST_ASSERT_EQUAL(3, s.n_fmts);
    TEST_ASSERT_EQUAL(3, s.n_rtpmap);
    TEST_ASSERT_EQUAL(20, s.ptime);
    TEST_ASSERT_EQUAL(SIP_DIR_SENDRECV, s.dir);

    sip_sdp_neg_t n;
    TEST_ASSERT_EQUAL(0, sip_sdp_negotiate(&s, prefs_ua, 2, true, true, &n));
    TEST_ASSERT_EQUAL(0, n.pt);
    TEST_ASSERT_EQUAL(101, n.dtmf_pt);
    TEST_ASSERT_EQUAL(12010, n.port);
    TEST_ASSERT_EQUAL(0, sip_sdp_negotiate(&s, prefs_au, 2, false, true, &n));
    TEST_ASSERT_EQUAL(8, n.pt); /* answerer: our preference wins */
    TEST_ASSERT_EQUAL(-1, n.dtmf_pt);
    TEST_ASSERT_EQUAL(0, sip_sdp_negotiate(&s, prefs_au, 2, true, false, &n));
    TEST_ASSERT_EQUAL(0, n.pt); /* answer to our offer: answerer's order wins */
}

static void test_media_level_overrides_and_bare_lf(void)
{
    TEST_ASSERT_EQUAL(0, parse("v=0\n"
                               "o=x 1 2 IN IP4 1.1.1.1\n"
                               "s=-\n"
                               "c=IN IP4 1.1.1.1\n"
                               "t=0 0\n"
                               "a=recvonly\n"
                               "m=video 5000 RTP/AVP 31\n"
                               "a=sendonly\n"
                               "m=audio 6000 RTP/AVP 8\n"
                               "c=IN IP4 2.2.2.2/127\n"));
    TEST_ASSERT_EQUAL(2, s.n_m);
    TEST_ASSERT_EQUAL(1, s.audio);
    TEST_ASSERT_EQUAL_HEX32(0x02020202, s.audio_ip);
    TEST_ASSERT_EQUAL(SIP_DIR_RECVONLY, s.dir); /* session level; video's attr ignored */
    sip_sdp_neg_t n;
    TEST_ASSERT_EQUAL(0, sip_sdp_negotiate(&s, prefs_ua, 2, true, true, &n));
    TEST_ASSERT_EQUAL(8, n.pt);
    TEST_ASSERT_EQUAL(-1, n.dtmf_pt);
}

static void test_hold_variants(void)
{
    sip_sdp_neg_t n;
    TEST_ASSERT_EQUAL(0, parse("v=0\r\no=x 1 3 IN IP4 1.1.1.1\r\ns=-\r\nc=IN IP4 0.0.0.0\r\nt=0 0\r\n"
                               "m=audio 6000 RTP/AVP 0\r\n"));
    TEST_ASSERT_TRUE(s.hold_ip);
    TEST_ASSERT_EQUAL(0, sip_sdp_negotiate(&s, prefs_ua, 2, true, true, &n));
    TEST_ASSERT_EQUAL(SIP_DIR_SENDONLY, n.remote_dir); /* remote won't receive */
    TEST_ASSERT_EQUAL(SIP_DIR_RECVONLY, sip_sdp_dir_combine(SIP_DIR_SENDRECV, n.remote_dir));

    TEST_ASSERT_EQUAL(0, parse("v=0\r\no=x 1 3 IN IP4 1.1.1.1\r\ns=-\r\nc=IN IP4 1.1.1.1\r\nt=0 0\r\n"
                               "m=audio 6000 RTP/AVP 0\r\na=inactive\r\n"));
    TEST_ASSERT_EQUAL(SIP_DIR_INACTIVE, s.dir);
    TEST_ASSERT_EQUAL(SIP_DIR_INACTIVE, sip_sdp_dir_combine(SIP_DIR_SENDRECV, s.dir));
    TEST_ASSERT_EQUAL(SIP_DIR_RECVONLY, sip_sdp_dir_combine(SIP_DIR_SENDRECV, SIP_DIR_SENDONLY));
    TEST_ASSERT_EQUAL(SIP_DIR_SENDONLY, sip_sdp_dir_combine(SIP_DIR_SENDRECV, SIP_DIR_RECVONLY));
    TEST_ASSERT_EQUAL(SIP_DIR_INACTIVE, sip_sdp_dir_combine(SIP_DIR_SENDONLY, SIP_DIR_SENDONLY));
    TEST_ASSERT_EQUAL(SIP_DIR_SENDONLY, sip_sdp_dir_combine(SIP_DIR_SENDONLY, SIP_DIR_SENDRECV));
}

static void test_no_common_codec(void)
{
    sip_sdp_neg_t n;
    TEST_ASSERT_EQUAL(0, parse("v=0\r\no=x 1 1 IN IP4 1.1.1.1\r\ns=-\r\nc=IN IP4 1.1.1.1\r\nt=0 0\r\n"
                               "m=audio 6000 RTP/AVP 18 9\r\na=rtpmap:18 G729/8000\r\n"));
    TEST_ASSERT_EQUAL(-1, sip_sdp_negotiate(&s, prefs_ua, 2, true, true, &n));
    /* PT 0 remapped to something else by rtpmap is not PCMU */
    TEST_ASSERT_EQUAL(0, parse("v=0\r\no=x 1 1 IN IP4 1.1.1.1\r\ns=-\r\nc=IN IP4 1.1.1.1\r\nt=0 0\r\n"
                               "m=audio 6000 RTP/AVP 0\r\na=rtpmap:0 opus/48000\r\n"));
    TEST_ASSERT_EQUAL(-1, sip_sdp_negotiate(&s, prefs_ua, 2, true, true, &n));
    /* audio port 0 = rejected stream; SAVP not supported */
    TEST_ASSERT_EQUAL(0, parse("v=0\r\no=x 1 1 IN IP4 1.1.1.1\r\ns=-\r\nc=IN IP4 1.1.1.1\r\nt=0 0\r\n"
                               "m=audio 0 RTP/AVP 0\r\nm=audio 7000 RTP/SAVP 0\r\n"));
    TEST_ASSERT_EQUAL(-1, s.audio);
    TEST_ASSERT_EQUAL(-1, sip_sdp_negotiate(&s, prefs_ua, 2, true, true, &n));
}

static void test_malformed(void)
{
    TEST_ASSERT_EQUAL(-1, parse(""));
    TEST_ASSERT_EQUAL(-1, parse("o=x 1 1 IN IP4 1.1.1.1\r\n"));           /* v= must come first */
    TEST_ASSERT_EQUAL(-1, parse("v=1\r\n"));
    TEST_ASSERT_EQUAL(-1, parse("v=0\r\nxyz\r\n"));
    TEST_ASSERT_EQUAL(-1, parse("v=0\r\nm=audio 6000 RTP/AVP 0\r\n"));   /* no c= */
    TEST_ASSERT_EQUAL(-1, parse("v=0\r\nc=IN IP6 ::1\r\nm=audio 6000 RTP/AVP 0\r\n"));
    TEST_ASSERT_EQUAL(-1, parse("v=0\r\nc=IN IP4 1.1.1.1\r\nm=audio 99999 RTP/AVP 0\r\n"));
    TEST_ASSERT_EQUAL(-1, parse("v=0\r\nc=IN IP4 1.1.1.1\r\nm=audio 6000 RTP/AVP 300\r\n"));
    TEST_ASSERT_EQUAL(-1, parse("v=0\r\nc=IN IP4 1.1.1.1\r\nm=audio 6000 RTP/AVP\r\n"));
    TEST_ASSERT_EQUAL(-1, parse("v=0\r\nc=IN IP4 1.1.1.1\r\nm=audio 6000 RTP/AVP 0\r\na=rtpmap:0 PCMU\r\n"));
    TEST_ASSERT_EQUAL(-1, parse("v=0\r\nc=IN IP4 1.1.1.999\r\nm=audio 6000 RTP/AVP 0\r\n"));
    TEST_ASSERT_EQUAL(-1, parse("v=0\r\nm=a 1 b c\r\nm=a 1 b c\r\nm=a 1 b c\r\nm=a 1 b c\r\nm=a 1 b c\r\n"));
    /* body without audio parses but has no audio */
    TEST_ASSERT_EQUAL(0, parse("v=0\r\nm=video 0 RTP/AVP 31\r\n"));
    TEST_ASSERT_EQUAL(-1, s.audio);
}

static void test_build_offer(void)
{
    char out[512];
    sip_sdp_local_t l = {
        .ip = 0x0a000002,
        .port = 4000,
        .sess_id = 42,
        .sess_ver = 1,
        .pts = prefs_ua,
        .n_pts = 2,
        .dtmf_pt = 101,
        .ptime = 20,
        .dir = SIP_DIR_SENDRECV,
    };
    int n = sip_sdp_build(out, sizeof(out), &l);
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_EQUAL_STRING("v=0\r\n"
                             "o=sipwire 42 1 IN IP4 10.0.0.2\r\n"
                             "s=sipwire\r\n"
                             "c=IN IP4 10.0.0.2\r\n"
                             "t=0 0\r\n"
                             "m=audio 4000 RTP/AVP 0 8 101\r\n"
                             "a=rtpmap:0 PCMU/8000\r\n"
                             "a=rtpmap:8 PCMA/8000\r\n"
                             "a=rtpmap:101 telephone-event/8000\r\n"
                             "a=fmtp:101 0-16\r\n"
                             "a=ptime:20\r\n"
                             "a=sendrecv\r\n",
                             out);
    /* our own offer parses back */
    TEST_ASSERT_EQUAL(0, sip_sdp_parse(&s, out, (size_t)n));
    TEST_ASSERT_EQUAL(4000, s.audio_port);
    TEST_ASSERT_EQUAL(-1, sip_sdp_build(out, 64, &l)); /* overflow detected */
}

static void test_build_answer_mirrors_mlines(void)
{
    TEST_ASSERT_EQUAL(0, parse("v=0\r\no=x 1 1 IN IP4 1.1.1.1\r\ns=-\r\nc=IN IP4 1.1.1.1\r\nt=0 0\r\n"
                               "m=video 5000 RTP/AVP 96\r\na=rtpmap:96 H264/90000\r\n"
                               "m=audio 6000 RTP/AVP 8 0 96\r\na=rtpmap:96 telephone-event/8000\r\n"
                               "a=sendonly\r\n"));
    sip_sdp_neg_t n;
    TEST_ASSERT_EQUAL(0, sip_sdp_negotiate(&s, prefs_ua, 2, true, true, &n));
    TEST_ASSERT_EQUAL(0, n.pt);
    TEST_ASSERT_EQUAL(96, n.dtmf_pt);
    uint8_t pt = n.pt;
    char out[512];
    sip_sdp_local_t l = {
        .ip = 0x0a000002, .port = 4000, .sess_id = 1, .sess_ver = 2,
        .pts = &pt, .n_pts = 1, .dtmf_pt = n.dtmf_pt, .ptime = 20,
        .dir = sip_sdp_dir_combine(SIP_DIR_SENDRECV, n.remote_dir),
        .offer = &s,
    };
    TEST_ASSERT_TRUE(sip_sdp_build(out, sizeof(out), &l) > 0);
    TEST_ASSERT_NOT_NULL(strstr(out, "m=video 0 RTP/AVP 96\r\nm=audio 4000 RTP/AVP 0 96\r\n"));
    TEST_ASSERT_NOT_NULL(strstr(out, "a=rtpmap:96 telephone-event/8000\r\n"));
    TEST_ASSERT_NOT_NULL(strstr(out, "a=recvonly\r\n"));
    TEST_ASSERT_NULL(strstr(out, "PCMA"));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_parse_typical);
    RUN_TEST(test_media_level_overrides_and_bare_lf);
    RUN_TEST(test_hold_variants);
    RUN_TEST(test_no_common_codec);
    RUN_TEST(test_malformed);
    RUN_TEST(test_build_offer);
    RUN_TEST(test_build_answer_mirrors_mlines);
    return UNITY_END();
}
