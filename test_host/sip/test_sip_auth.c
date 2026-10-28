/* MD5 (RFC 1321 test suite) and digest authentication (RFC 2617). */

#include <stdio.h>
#include <string.h>

#include "sip_auth.h"
#include "sip_md5.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static void test_md5_rfc1321_vectors(void)
{
    static const struct {
        const char *in;
        const char *out;
    } v[] = {
        { "", "d41d8cd98f00b204e9800998ecf8427e" },
        { "a", "0cc175b9c0f1b6a831c399e269772661" },
        { "abc", "900150983cd24fb0d6963f7d28e17f72" },
        { "message digest", "f96b697d7cb7938d525a2f31aaf161d0" },
        { "abcdefghijklmnopqrstuvwxyz", "c3fcd3d76192e4007dfb496cca67e13b" },
        { "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789",
          "d174ab98d277d9f5a5611c2c9f419d9f" },
        { "12345678901234567890123456789012345678901234567890123456789012345678901234567890",
          "57edf4a22be3c955ac49da2e2107b67a" },
    };
    char hex[33];
    for (size_t i = 0; i < sizeof(v) / sizeof(v[0]); i++) {
        sip_md5_hex(v[i].in, strlen(v[i].in), hex);
        TEST_ASSERT_EQUAL_STRING(v[i].out, hex);
    }
}

static void test_md5_incremental(void)
{
    /* feed the 80-byte vector in awkward chunk sizes across block borders */
    const char *s = "12345678901234567890123456789012345678901234567890123456789012345678901234567890";
    for (size_t chunk = 1; chunk <= 80; chunk++) {
        sip_md5_ctx_t c;
        uint8_t d[16];
        char hex[33];
        sip_md5_init(&c);
        for (size_t off = 0; off < 80; off += chunk) {
            size_t n = 80 - off < chunk ? 80 - off : chunk;
            sip_md5_update(&c, s + off, n);
        }
        sip_md5_final(&c, d);
        sip_md5_to_hex(d, hex);
        TEST_ASSERT_EQUAL_STRING("57edf4a22be3c955ac49da2e2107b67a", hex);
    }
    /* 55/56/64 byte lengths exercise the padding edge cases */
    char buf[64];
    char hex[33];
    memset(buf, 'a', sizeof(buf));
    sip_md5_hex(buf, 55, hex);
    TEST_ASSERT_EQUAL_STRING("ef1772b6dff9a122358552954ad0df65", hex);
    sip_md5_hex(buf, 56, hex);
    TEST_ASSERT_EQUAL_STRING("3b0c8ac703f828b04c6c197006d17218", hex);
    sip_md5_hex(buf, 64, hex);
    TEST_ASSERT_EQUAL_STRING("014842d480b571495a4a0363793f7367", hex);
}

/* RFC 2617 §3.5 example */
static void test_digest_rfc2617_vector(void)
{
    char resp[33];
    sip_digest_response("Mufasa", "testrealm@host.com", "Circle Of Life", "GET", "/dir/index.html",
                        "dcd98b7102dd2f0e8b11d0f600bfb0c093", "00000001", "0a4f113b", "auth", resp);
    TEST_ASSERT_EQUAL_STRING("6629fae49393a05397450978507c4ef1", resp);
}

static void test_digest_legacy_no_qop(void)
{
    char resp[33], ha1[33], ha2[33], exp[33], tmp[256];
    sip_digest_response("1001", "asterisk", "pw", "REGISTER", "sip:10.0.0.1", "abc", NULL, NULL, NULL, resp);
    sip_md5_hex("1001:asterisk:pw", 16, ha1);
    sip_md5_hex("REGISTER:sip:10.0.0.1", 21, ha2);
    int n = snprintf(tmp, sizeof(tmp), "%s:abc:%s", ha1, ha2);
    sip_md5_hex(tmp, (size_t)n, exp);
    TEST_ASSERT_EQUAL_STRING(exp, resp);
}

static void test_parse_challenge(void)
{
    sip_auth_t a;
    TEST_ASSERT_EQUAL(0, sip_auth_parse_challenge(
                             sip_str_c("Digest realm=\"asterisk\",nonce=\"16\\\"x\",opaque=\"o1\","
                                       "algorithm=md5,qop=\"auth,auth-int\", stale=TRUE"),
                             false, &a));
    TEST_ASSERT_TRUE(a.valid);
    TEST_ASSERT_EQUAL_STRING("asterisk", a.realm);
    TEST_ASSERT_EQUAL_STRING("16\"x", a.nonce);
    TEST_ASSERT_EQUAL_STRING("o1", a.opaque);
    TEST_ASSERT_TRUE(a.has_opaque);
    TEST_ASSERT_TRUE(a.qop_auth);
    TEST_ASSERT_TRUE(a.stale);
    TEST_ASSERT_FALSE(a.proxy);

    TEST_ASSERT_EQUAL(0, sip_auth_parse_challenge(sip_str_c("digest nonce=\"n\", realm=r"), true, &a));
    TEST_ASSERT_FALSE(a.qop_auth);
    TEST_ASSERT_TRUE(a.proxy);
    TEST_ASSERT_EQUAL_STRING("r", a.realm);

    TEST_ASSERT_EQUAL(-1, sip_auth_parse_challenge(sip_str_c("Basic realm=\"x\""), false, &a));
    TEST_ASSERT_EQUAL(-1, sip_auth_parse_challenge(sip_str_c("Digest realm=\"x\""), false, &a));
    TEST_ASSERT_EQUAL(-1, sip_auth_parse_challenge(sip_str_c("Digest realm=\"x\",nonce=\"y\",algorithm=SHA-256"),
                                                   false, &a));
    TEST_ASSERT_EQUAL(-1, sip_auth_parse_challenge(sip_str_c("Digest realm=\"x\",nonce=\"y\",qop=\"auth-int\""),
                                                   false, &a));
    TEST_ASSERT_EQUAL(-1, sip_auth_parse_challenge(sip_str_c("Digest realm=\"x,nonce=\"y\""), false, &a));
    TEST_ASSERT_EQUAL(-1, sip_auth_parse_challenge(sip_str_c("Digest realm"), false, &a));
    char longnonce[400];
    memset(longnonce, 'n', sizeof(longnonce));
    snprintf(longnonce, sizeof(longnonce), "Digest realm=\"x\",nonce=\"%0300d\"", 0);
    TEST_ASSERT_EQUAL(-1, sip_auth_parse_challenge(sip_str_c(longnonce), false, &a));
}

static void test_pick_md5_among_challenges(void)
{
    static char buf[] =
        "SIP/2.0 401 Unauthorized\r\n"
        "Via: SIP/2.0/UDP 10.0.0.2;branch=z9hG4bK1\r\n"
        "From: <sip:a@x>;tag=1\r\nTo: <sip:a@x>;tag=2\r\nCall-ID: c\r\nCSeq: 1 REGISTER\r\n"
        "WWW-Authenticate: Digest realm=\"x\",nonce=\"sha\",algorithm=SHA-256,qop=\"auth\"\r\n"
        "WWW-Authenticate: Digest realm=\"x\",nonce=\"md5\",algorithm=MD5,qop=\"auth\"\r\n"
        "Content-Length: 0\r\n\r\n";
    sip_msg_t m;
    sip_auth_t a;
    TEST_ASSERT_EQUAL(SIP_PARSE_OK, sip_msg_parse(&m, buf, strlen(buf)));
    TEST_ASSERT_EQUAL(0, sip_auth_from_response(&m, &a));
    TEST_ASSERT_EQUAL_STRING("md5", a.nonce);
}

static void test_write_header(void)
{
    sip_auth_t a;
    TEST_ASSERT_EQUAL(0, sip_auth_parse_challenge(
                             sip_str_c("Digest realm=\"testrealm@host.com\", qop=\"auth\", "
                                       "nonce=\"dcd98b7102dd2f0e8b11d0f600bfb0c093\", "
                                       "opaque=\"5ccc069c403ebaf9f0171e9517f40e41\""),
                             false, &a));
    char out[512];
    sip_wbuf_t w;
    sip_w_init(&w, out, sizeof(out));
    sip_auth_write_header(&w, &a, "Mufasa", "Circle Of Life", "GET", "/dir/index.html", "0a4f113b");
    TEST_ASSERT_FALSE(w.overflow);
    TEST_ASSERT_EQUAL_STRING(
        "Authorization: Digest username=\"Mufasa\", realm=\"testrealm@host.com\", "
        "nonce=\"dcd98b7102dd2f0e8b11d0f600bfb0c093\", uri=\"/dir/index.html\", "
        "response=\"6629fae49393a05397450978507c4ef1\", algorithm=MD5, qop=auth, nc=00000001, "
        "cnonce=\"0a4f113b\", opaque=\"5ccc069c403ebaf9f0171e9517f40e41\"\r\n",
        out);
    /* reuse of the nonce increments nc */
    sip_w_init(&w, out, sizeof(out));
    sip_auth_write_header(&w, &a, "Mufasa", "Circle Of Life", "GET", "/dir/index.html", "0a4f113b");
    TEST_ASSERT_NOT_NULL(strstr(out, "nc=00000002"));
    TEST_ASSERT_NULL(strstr(out, "Circle Of Life"));
    a.proxy = true;
    sip_w_init(&w, out, sizeof(out));
    sip_auth_write_header(&w, &a, "Mufasa", "Circle Of Life", "GET", "/dir/index.html", "0a4f113b");
    TEST_ASSERT_EQUAL(0, strncmp(out, "Proxy-Authorization: Digest ", 28));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_md5_rfc1321_vectors);
    RUN_TEST(test_md5_incremental);
    RUN_TEST(test_digest_rfc2617_vector);
    RUN_TEST(test_digest_legacy_no_qop);
    RUN_TEST(test_parse_challenge);
    RUN_TEST(test_pick_md5_among_challenges);
    RUN_TEST(test_write_header);
    return UNITY_END();
}
