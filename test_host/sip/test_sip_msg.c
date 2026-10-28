/* Parser / serializer tests, incl. the RFC 4475 torture cases that matter
 * for a UA. */

#include <stdio.h>
#include <string.h>

#include "sip_dialog.h"
#include "sip_msg.h"
#include "unity.h"

static char buf[4096];
static sip_msg_t m;

void setUp(void) {}
void tearDown(void) {}

static sip_parse_result_t parse(const char *s)
{
    size_t n = strlen(s);
    TEST_ASSERT_TRUE(n < sizeof(buf));
    memcpy(buf, s, n + 1);
    return sip_msg_parse(&m, buf, n);
}

/* builds "<hdrs>Content-Length: N\r\n\r\n<body>" */
static sip_parse_result_t parse_with_body(const char *hdrs, const char *body)
{
    static char tmp[4096];
    snprintf(tmp, sizeof(tmp), "%sContent-Length: %u\r\n\r\n%s", hdrs, (unsigned)strlen(body), body);
    return parse(tmp);
}

static void assert_str(const char *exp, sip_str_t s)
{
    char tmp[512];
    TEST_ASSERT_TRUE(s.len < sizeof(tmp));
    memcpy(tmp, s.p ? s.p : "", s.len);
    tmp[s.len] = '\0';
    TEST_ASSERT_EQUAL_STRING(exp, tmp);
}

#define BASIC_HDRS                                                   \
    "Via: SIP/2.0/UDP 10.0.0.1:5060;branch=z9hG4bKabc\r\n"           \
    "From: <sip:a@x>;tag=1\r\n"                                      \
    "To: <sip:b@x>\r\n"                                              \
    "Call-ID: cid@x\r\n"                                             \
    "CSeq: 1 INVITE\r\n"

static void test_basic_request(void)
{
    TEST_ASSERT_EQUAL(SIP_PARSE_OK, parse("INVITE sip:bob@example.com SIP/2.0\r\n" BASIC_HDRS
                                          "Max-Forwards: 70\r\n"
                                          "Content-Length: 0\r\n\r\n"));
    TEST_ASSERT_TRUE(m.is_request);
    TEST_ASSERT_EQUAL(SIP_METHOD_INVITE, m.method);
    assert_str("sip:bob@example.com", m.ruri);
    TEST_ASSERT_EQUAL(1, m.n_via);
    assert_str("z9hG4bKabc", m.via[0].branch);
    assert_str("10.0.0.1", m.via[0].host);
    TEST_ASSERT_EQUAL(5060, m.via[0].port);
    TEST_ASSERT_EQUAL(-1, m.via[0].rport);
    assert_str("1", m.from.tag);
    TEST_ASSERT_EQUAL(0, m.to.tag.len);
    assert_str("cid@x", m.call_id);
    TEST_ASSERT_EQUAL_UINT32(1, m.cseq);
    TEST_ASSERT_EQUAL(SIP_METHOD_INVITE, m.cseq_method);
    TEST_ASSERT_EQUAL(70, m.max_forwards);
    TEST_ASSERT_EQUAL(0, m.content_length);
    TEST_ASSERT_EQUAL(0, m.body.len);
}

static void test_basic_response(void)
{
    TEST_ASSERT_EQUAL(SIP_PARSE_OK,
                      parse("SIP/2.0 401 Unauthorized\r\n"
                            "Via: SIP/2.0/UDP 10.0.0.2:5060;rport=5062;received=1.2.3.4;branch=z9hG4bKx\r\n"
                            "From: <sip:a@x>;tag=1\r\nTo: <sip:a@x>;tag=srv\r\nCall-ID: c\r\n"
                            "CSeq: 2 REGISTER\r\n"
                            "WWW-Authenticate: Digest realm=\"asterisk\",nonce=\"abc\"\r\n"
                            "Content-Length: 0\r\n\r\n"));
    TEST_ASSERT_FALSE(m.is_request);
    TEST_ASSERT_EQUAL(401, m.status);
    assert_str("Unauthorized", m.reason);
    TEST_ASSERT_EQUAL(5062, m.via[0].rport);
    assert_str("1.2.3.4", m.via[0].received);
    TEST_ASSERT_EQUAL(SIP_METHOD_REGISTER, m.cseq_method);
    TEST_ASSERT_EQUAL(1, m.n_www_auth);
    assert_str("srv", m.to.tag);
}

/* RFC 4475 3.1.1.1 "wsinv": short tortuous INVITE */
static void test_rfc4475_wsinv(void)
{
    const char *hdrs =
        "INVITE sip:vivekg@chair-dnrc.example.com;unknownparam SIP/2.0\r\n"
        "TO :\r\n"
        " sip:vivekg@chair-dnrc.example.com ;   tag    = 1918181833n\r\n"
        "from   : \"J Rosenberg \\\\\\\"\"       <sip:jdrosen@example.com>\r\n"
        "  ;\r\n"
        "  tag = 98asjd8\r\n"
        "MaX-fOrWaRdS: 0068\r\n"
        "Call-ID: wsinv.ndaksdj@192.0.2.1\r\n"
        "cseq: 0009\r\n"
        "  INVITE\r\n"
        "Via  : SIP  /   2.0\r\n"
        " /UDP\r\n"
        "    192.0.2.2;branch=390skdjuw\r\n"
        "s :\r\n"
        "NewFangledHeader:   newfangled value\r\n"
        " continued newfangled value\r\n"
        "UnknownHeaderWithUnusualValue: ;;,,;;,;\r\n"
        "Content-Type: application/sdp\r\n"
        "Route:\r\n"
        " <sip:services.example.com;lr;unknownwith=value;unknown-no-value>\r\n"
        "v:  SIP  / 2.0  / TCP     spindle.example.com   ;\r\n"
        "  branch  =   z9hG4bK9ikj8  ,\r\n"
        " SIP  /    2.0   / UDP  192.168.255.111   ; branch=\r\n"
        " z9hG4bK30239\r\n"
        "m:\"Quoted string \\\"\\\"\" <sip:jdrosen@example.com> ; newparam =\r\n"
        "      newvalue ;\r\n"
        "  secondparam ; q = 0.33\r\n";
    const char *body =
        "v=0\r\n"
        "o=mhandley 29739 7272939 IN IP4 192.0.2.3\r\n"
        "s=-\r\n"
        "c=IN IP4 192.0.2.4\r\n"
        "t=0 0\r\n"
        "m=audio 49217 RTP/AVP 0 12\r\n"
        "m=video 3227 RTP/AVP 31\r\n"
        "a=rtpmap:31 LPC\r\n";
    TEST_ASSERT_EQUAL(SIP_PARSE_OK, parse_with_body(hdrs, body));
    assert_str("1918181833n", m.to.tag);
    assert_str("sip:vivekg@chair-dnrc.example.com", m.to.uri);
    assert_str("98asjd8", m.from.tag);
    assert_str("J Rosenberg \\\\\\\"", m.from.display);
    TEST_ASSERT_EQUAL(68, m.max_forwards);
    TEST_ASSERT_EQUAL_UINT32(9, m.cseq);
    TEST_ASSERT_EQUAL(SIP_METHOD_INVITE, m.cseq_method);
    TEST_ASSERT_EQUAL(3, m.n_via);
    assert_str("390skdjuw", m.via[0].branch);
    assert_str("192.0.2.2", m.via[0].host);
    assert_str("TCP", m.via[1].transport);
    assert_str("spindle.example.com", m.via[1].host);
    assert_str("z9hG4bK9ikj8", m.via[1].branch);
    assert_str("192.168.255.111", m.via[2].host);
    assert_str("z9hG4bK30239", m.via[2].branch);
    TEST_ASSERT_EQUAL(1, m.n_route);
    assert_str("sip:services.example.com;lr;unknownwith=value;unknown-no-value", m.route[0].uri);
    TEST_ASSERT_EQUAL(1, m.n_contact);
    assert_str("Quoted string \\\"\\\"", m.contact[0].display);
    TEST_ASSERT_EQUAL(strlen(body), m.body.len);
    sip_uri_t u;
    TEST_ASSERT_EQUAL(0, sip_uri_parse(m.route[0].uri, &u));
    TEST_ASSERT_TRUE(u.lr);
}

/* RFC 4475 3.1.1.2 "intmeth" (minus the raw control characters, which this
 * parser deliberately refuses — see test_nul_rejected) */
static void test_rfc4475_intmeth(void)
{
    TEST_ASSERT_EQUAL(SIP_PARSE_OK,
        parse("!interesting-Method0123456789_*+`.%indeed'~ "
              "sip:1_unusual.URI~(to-be!sure)&isn't+it$/crazy?,/;;*:&it+has=1,weird!*pas$wo~d_too."
              "(doesn't-it)@example.com SIP/2.0\r\n"
              "Via: SIP/2.0/TCP host1.example.com;branch=z9hG4bK-.!%66*_+`'~\r\n"
              "To: \"BEL: NUL: DEL:\" <sip:1_unusual.URI~(to-be!sure)&isn't+it$/crazy?,/;;*@example.com>\r\n"
              "From: token1~` token2'+_ token3*%!.- <sip:mundane@example.com>;fromParam''~+*_!.-%="
              "\"\xd1\x80\xd0\xb0\xd0\xb1\xd0\xbe\";tag=_token~1'+`*%!-.\r\n"
              "Call-ID: intmeth.word%ZK-!.*_+'@word`~)(><:\\/\"][?}{\r\n"
              "CSeq: 139122385 !interesting-Method0123456789_*+`.%indeed'~\r\n"
              "Max-Forwards: 255\r\n"
              "extensionHeader-!.%*+_`'~:\xef\xbb\xbf\xe5\xa4\xa7\xe5\x81\x9c\xe9\x9b\xbb\r\n"
              "Content-Length: 0\r\n\r\n"));
    TEST_ASSERT_EQUAL(SIP_METHOD_UNKNOWN, m.method);
    assert_str("!interesting-Method0123456789_*+`.%indeed'~", m.method_str);
    assert_str("token1~` token2'+_ token3*%!.-", m.from.display);
    assert_str("_token~1'+`*%!-.", m.from.tag);
    assert_str("z9hG4bK-.!%66*_+`'~", m.via[0].branch);
    sip_uri_t u;
    TEST_ASSERT_EQUAL(0, sip_uri_parse(m.ruri, &u));
    assert_str("example.com", u.host);
    assert_str("1_unusual.URI~(to-be!sure)&isn't+it$/crazy?,/;;*", u.user);
}

/* RFC 4475 3.1.1.3 "esc01": escaped characters in URIs */
static void test_rfc4475_esc01(void)
{
    TEST_ASSERT_EQUAL(SIP_PARSE_OK,
        parse("INVITE sip:sips%3Auser%40example.com@example.net SIP/2.0\r\n"
              "To: sip:%75se%72@example.com\r\n"
              "From: <sip:I%20have%20spaces@example.net>;tag=938\r\n"
              "Max-Forwards: 87\r\n"
              "i: esc01.239409asdfakjkn23onasd0-3234\r\n"
              "CSeq: 234234 INVITE\r\n"
              "Via: SIP/2.0/UDP host5.example.net;branch=z9hG4bKkdjuw\r\n"
              "C: application/sdp\r\n"
              "Contact:\r\n"
              "  <sip:cal%6Cer@host5.example.net;%6C%72;n%61me=v%61lue%25%34%31>\r\n"
              "Content-Length: 0\r\n\r\n"));
    sip_uri_t u;
    char user[64];
    TEST_ASSERT_EQUAL(0, sip_uri_parse(m.ruri, &u));
    TEST_ASSERT_EQUAL(21, sip_unescape(user, sizeof(user), u.user));
    TEST_ASSERT_EQUAL_STRING("sips:user@example.com", user);
    TEST_ASSERT_EQUAL(0, sip_uri_parse(m.to.uri, &u));
    sip_unescape(user, sizeof(user), u.user);
    TEST_ASSERT_EQUAL_STRING("user", user);
    TEST_ASSERT_EQUAL(0, sip_uri_parse(m.from.uri, &u));
    sip_unescape(user, sizeof(user), u.user);
    TEST_ASSERT_EQUAL_STRING("I have spaces", user);
    assert_str("application/sdp", m.content_type);
    TEST_ASSERT_EQUAL(1, m.n_contact);
}

/* RFC 4475 3.1.1.4 "escnull": %00 must not be unescaped into a C string */
static void test_escnull(void)
{
    char out[32];
    TEST_ASSERT_EQUAL(-1, sip_unescape(out, sizeof(out), sip_str_c("null-%00-null")));
    TEST_ASSERT_EQUAL(-1, sip_unescape(out, sizeof(out), sip_str_c("bad%4")));
    TEST_ASSERT_EQUAL(-1, sip_unescape(out, sizeof(out), sip_str_c("bad%zz")));
    TEST_ASSERT_EQUAL(-1, sip_unescape(out, 4, sip_str_c("toolong")));
}

static void test_compact_forms(void)
{
    TEST_ASSERT_EQUAL(SIP_PARSE_ERR,
        parse_with_body("INVITE sip:b@x SIP/2.0\r\n"
                        "v: SIP/2.0/UDP h1;branch=z9hG4bK1\r\n"
                        "f: \"A\" <sip:a@x>;tag=f\r\n"
                        "t: sip:b@x\r\n"
                        "i: compact@x\r\n"
                        "CSeq: 5 INVITE\r\n"
                        "m: <sip:a@1.2.3.4:5070>;expires=60\r\n"
                        "c: application/sdp\r\n"
                        "k: timer\r\n"
                        "s: subject is ignored\r\n"
                        "l: 4\r\n", "v=0\r\n"));
    /* compact "l" and long "Content-Length" are the same header: duplicate */
}

static void test_compact_forms_ok(void)
{
    TEST_ASSERT_EQUAL(SIP_PARSE_OK,
        parse("INVITE sip:b@x SIP/2.0\r\n"
              "v: SIP/2.0/UDP h1;branch=z9hG4bK1\r\n"
              "f: \"A\" <sip:a@x>;tag=f\r\n"
              "t: sip:b@x\r\n"
              "i: compact@x\r\n"
              "CSeq: 5 INVITE\r\n"
              "m: <sip:a@1.2.3.4:5070>;expires=60\r\n"
              "c: application/sdp\r\n"
              "k: timer\r\n"
              "s: subject is ignored\r\n"
              "l: 5\r\n\r\nv=0\r\n"));
    assert_str("compact@x", m.call_id);
    assert_str("f", m.from.tag);
    assert_str("A", m.from.display);
    assert_str("sip:b@x", m.to.uri);
    TEST_ASSERT_EQUAL(1, m.n_contact);
    TEST_ASSERT_EQUAL(60, m.contact[0].expires);
    assert_str("application/sdp", m.content_type);
    assert_str("timer", m.supported);
    TEST_ASSERT_EQUAL(5, m.content_length);
    assert_str("v=0\r\n", m.body);
}

static void test_duplicate_content_length_rejected(void)
{
    TEST_ASSERT_EQUAL(SIP_PARSE_ERR,
        parse("INVITE sip:b@x SIP/2.0\r\n" BASIC_HDRS "Content-Length: 0\r\nl: 0\r\n\r\n"));
}

static void test_multiple_via_and_contact_lists(void)
{
    TEST_ASSERT_EQUAL(SIP_PARSE_OK,
        parse("SIP/2.0 200 OK\r\n"
              "Via: SIP/2.0/UDP a.example.com;branch=z9hG4bK1, SIP/2.0/UDP b.example.com:5070;branch=z9hG4bK2\r\n"
              "Via: SIP/2.0/UDP [2001:db8::1]:5080;branch=z9hG4bK3;rport\r\n"
              "From: <sip:a@x>;tag=1\r\nTo: <sip:a@x>;tag=2\r\nCall-ID: c\r\nCSeq: 1 REGISTER\r\n"
              "Contact: <sip:1001@10.0.0.2:5060>;expires=120, \"x,y\" <sip:1001@10.0.0.9;a=b,c>;expires=30\r\n"
              "Record-Route: <sip:p1;lr>, <sip:p2;lr>\r\nRecord-Route: <sip:p3>\r\n"
              "Content-Length: 0\r\n\r\n"));
    TEST_ASSERT_EQUAL(3, m.n_via);
    assert_str("b.example.com", m.via[1].host);
    TEST_ASSERT_EQUAL(5070, m.via[1].port);
    assert_str("[2001:db8::1]", m.via[2].host);
    TEST_ASSERT_EQUAL(5080, m.via[2].port);
    TEST_ASSERT_EQUAL(0, m.via[2].rport);
    TEST_ASSERT_EQUAL(2, m.n_contact);
    TEST_ASSERT_EQUAL(120, m.contact[0].expires);
    TEST_ASSERT_EQUAL(30, m.contact[1].expires);
    assert_str("sip:1001@10.0.0.9;a=b,c", m.contact[1].uri);
    TEST_ASSERT_EQUAL(3, m.n_rr);
    assert_str("<sip:p2;lr>", m.rr[1].raw);
}

static void test_too_many_vias_rejected(void)
{
    char tmp[2048];
    int n = snprintf(tmp, sizeof(tmp), "SIP/2.0 200 OK\r\n");
    for (int i = 0; i < SIP_MAX_VIA + 1; i++) {
        n += snprintf(tmp + n, sizeof(tmp) - (size_t)n, "Via: SIP/2.0/UDP h%d;branch=z9hG4bK%d\r\n", i, i);
    }
    snprintf(tmp + n, sizeof(tmp) - (size_t)n,
             "From: <sip:a@x>;tag=1\r\nTo: <sip:a@x>\r\nCall-ID: c\r\nCSeq: 1 BYE\r\n\r\n");
    TEST_ASSERT_EQUAL(SIP_PARSE_ERR, parse(tmp));
}

static void test_unknown_headers_and_bare_lf(void)
{
    TEST_ASSERT_EQUAL(SIP_PARSE_OK,
        parse("OPTIONS sip:b@x SIP/2.0\n"
              "Via: SIP/2.0/UDP h;branch=z9hG4bK1\n"
              "X-Weird-Header: ;;,,\"unbalanced in unknown header is fine\n"
              "From: <sip:a@x>;tag=1\nTo: <sip:b@x>\nCall-ID: c\nCSeq: 1 OPTIONS\n"
              "P-Asserted-Identity: <sip:x@y>\n\n"));
    TEST_ASSERT_EQUAL(SIP_METHOD_OPTIONS, m.method);
    TEST_ASSERT_EQUAL(-1, m.content_length);
    TEST_ASSERT_EQUAL(0, m.body.len);
}

static void test_content_length_handling(void)
{
    /* body shorter than Content-Length (RFC 4475 3.1.2.2 "clerr") */
    TEST_ASSERT_EQUAL(SIP_PARSE_ERR,
        parse("INVITE sip:b@x SIP/2.0\r\n" BASIC_HDRS "Content-Length: 9999\r\n\r\nv=0\r\n"));
    /* negative ("scalar02") */
    TEST_ASSERT_EQUAL(SIP_PARSE_ERR,
        parse("INVITE sip:b@x SIP/2.0\r\n" BASIC_HDRS "Content-Length: -999\r\n\r\n"));
    /* garbage */
    TEST_ASSERT_EQUAL(SIP_PARSE_ERR,
        parse("INVITE sip:b@x SIP/2.0\r\n" BASIC_HDRS "Content-Length: 1x\r\n\r\n"));
    /* longer datagram: extra bytes discarded (§18.3) */
    TEST_ASSERT_EQUAL(SIP_PARSE_OK,
        parse("INVITE sip:b@x SIP/2.0\r\n" BASIC_HDRS "Content-Length: 3\r\n\r\nabcdef"));
    assert_str("abc", m.body);
    /* absent on UDP: rest of datagram */
    TEST_ASSERT_EQUAL(SIP_PARSE_OK, parse("INVITE sip:b@x SIP/2.0\r\n" BASIC_HDRS "\r\nxyz"));
    assert_str("xyz", m.body);
}

static void test_oversized_rejected(void)
{
    static char big[SIP_MAX_MSG_IN + 64];
    memset(big, 'a', sizeof(big));
    TEST_ASSERT_EQUAL(SIP_PARSE_TOO_BIG, sip_msg_parse(&m, big, SIP_MAX_MSG_IN + 1));
}

static void test_missing_mandatory_rejected(void)
{
    /* RFC 4475 3.1.2.15 "insuf" */
    TEST_ASSERT_EQUAL(SIP_PARSE_ERR,
        parse("INVITE sip:user@example.com SIP/2.0\r\nCSeq: 193942 INVITE\r\n"
              "Via: SIP/2.0/UDP 192.0.2.95;branch=z9hG4bKkdj.insuf\r\n"
              "Content-Type: application/sdp\r\nl: 0\r\n\r\n"));
    TEST_ASSERT_EQUAL(SIP_PARSE_ERR,
        parse("INVITE sip:b@x SIP/2.0\r\nFrom: <sip:a@x>;tag=1\r\nTo: <sip:b@x>\r\n"
              "Call-ID: c\r\nCSeq: 1 INVITE\r\n\r\n")); /* no Via */
}

static void test_duplicate_single_headers_rejected(void)
{
    /* RFC 4475 3.1.2.? "multi01" */
    TEST_ASSERT_EQUAL(SIP_PARSE_ERR, parse("INVITE sip:b@x SIP/2.0\r\n" BASIC_HDRS "Call-ID: other\r\n\r\n"));
    TEST_ASSERT_EQUAL(SIP_PARSE_ERR, parse("INVITE sip:b@x SIP/2.0\r\n" BASIC_HDRS "To: <sip:c@x>\r\n\r\n"));
    TEST_ASSERT_EQUAL(SIP_PARSE_ERR, parse("INVITE sip:b@x SIP/2.0\r\n" BASIC_HDRS "From: <sip:c@x>\r\n\r\n"));
    TEST_ASSERT_EQUAL(SIP_PARSE_ERR, parse("INVITE sip:b@x SIP/2.0\r\n" BASIC_HDRS "CSeq: 2 INVITE\r\n\r\n"));
}

static void test_bad_start_lines(void)
{
    /* "lwsstart" */
    TEST_ASSERT_EQUAL(SIP_PARSE_ERR, parse("INVITE  sip:b@x  SIP/2.0\r\n" BASIC_HDRS "\r\n"));
    /* "trws" */
    TEST_ASSERT_EQUAL(SIP_PARSE_ERR, parse("INVITE sip:b@x SIP/2.0  \r\n" BASIC_HDRS "\r\n"));
    /* "badvers" */
    TEST_ASSERT_EQUAL(SIP_PARSE_ERR, parse("INVITE sip:b@x SIP/7.0\r\n" BASIC_HDRS "\r\n"));
    /* "ltgtruri" */
    TEST_ASSERT_EQUAL(SIP_PARSE_ERR, parse("INVITE <sip:b@x> SIP/2.0\r\n" BASIC_HDRS "\r\n"));
    /* "lwsruri" */
    TEST_ASSERT_EQUAL(SIP_PARSE_ERR, parse("INVITE sip:b@x; lr SIP/2.0\r\n" BASIC_HDRS "\r\n"));
    /* "bigcode" */
    TEST_ASSERT_EQUAL(SIP_PARSE_ERR, parse("SIP/2.0 4294967301 better not break the receiver\r\n" BASIC_HDRS "\r\n"));
    TEST_ASSERT_EQUAL(SIP_PARSE_ERR, parse("SIP/2.0 700 Nope\r\n" BASIC_HDRS "\r\n"));
    TEST_ASSERT_EQUAL(SIP_PARSE_ERR, parse("SIP/2.0 099 Nope\r\n" BASIC_HDRS "\r\n"));
    /* folded start line */
    TEST_ASSERT_EQUAL(SIP_PARSE_ERR, parse("INVITE sip:b@x\r\n SIP/2.0\r\n" BASIC_HDRS "\r\n"));
    /* no blank line */
    TEST_ASSERT_EQUAL(SIP_PARSE_ERR, parse("INVITE sip:b@x SIP/2.0\r\n" BASIC_HDRS));
    /* "mismatch01": CSeq method differs */
    TEST_ASSERT_EQUAL(SIP_PARSE_ERR, parse("OPTIONS sip:b@x SIP/2.0\r\n" BASIC_HDRS "\r\n"));
    /* "noreason" / unicode reason are fine */
    TEST_ASSERT_EQUAL(SIP_PARSE_OK, parse("SIP/2.0 100 \r\n" BASIC_HDRS "\r\n"));
    TEST_ASSERT_EQUAL(0, m.reason.len);
    TEST_ASSERT_EQUAL(SIP_PARSE_OK, parse("SIP/2.0 200 = 2**3 * 5**2 \xd0\xbd\xd0\xbe\r\n" BASIC_HDRS "\r\n"));
    TEST_ASSERT_EQUAL(200, m.status);
}

static void test_scalar_overflow_rejected(void)
{
    /* RFC 4475 "scalarlg" */
    TEST_ASSERT_EQUAL(SIP_PARSE_ERR,
        parse("REGISTER sip:x SIP/2.0\r\nVia: SIP/2.0/UDP h;branch=z9hG4bK1\r\n"
              "From: <sip:a@x>;tag=1\r\nTo: <sip:a@x>\r\nCall-ID: c\r\n"
              "CSeq: 36893488147419103232 REGISTER\r\n\r\n"));
    TEST_ASSERT_EQUAL(SIP_PARSE_ERR,
        parse("INVITE sip:b@x SIP/2.0\r\n" BASIC_HDRS "Max-Forwards: 300\r\n\r\n"));
    TEST_ASSERT_EQUAL(SIP_PARSE_ERR,
        parse("INVITE sip:b@x SIP/2.0\r\n" BASIC_HDRS "Expires: 4294967297\r\n\r\n"));
    TEST_ASSERT_EQUAL(SIP_PARSE_ERR,
        parse("INVITE sip:b@x SIP/2.0\r\n" BASIC_HDRS
              "Contact: <sip:a@x>;expires=280297596632815\r\n\r\n"));
    /* CSeq must be < 2^31 */
    TEST_ASSERT_EQUAL(SIP_PARSE_ERR,
        parse("BYE sip:b@x SIP/2.0\r\nVia: SIP/2.0/UDP h;branch=z9hG4bK1\r\n"
              "From: <sip:a@x>;tag=1\r\nTo: <sip:a@x>\r\nCall-ID: c\r\nCSeq: 2147483648 BYE\r\n\r\n"));
}

static void test_quoting_and_separators(void)
{
    /* "quotbal": unterminated quote */
    TEST_ASSERT_EQUAL(SIP_PARSE_ERR,
        parse("INVITE sip:b@x SIP/2.0\r\nVia: SIP/2.0/UDP h;branch=z9hG4bK1\r\n"
              "From: \"Bell, Alexander <sip:a.g.bell@example.com>;tag=43\r\n"
              "To: <sip:b@x>\r\nCall-ID: c\r\nCSeq: 1 INVITE\r\n\r\n"));
    /* "badinv01": extraneous separators */
    TEST_ASSERT_EQUAL(SIP_PARSE_ERR,
        parse("INVITE sip:b@x SIP/2.0\r\nVia: SIP/2.0/UDP 192.0.2.15;;,;,,\r\n"
              "From: <sip:a@x>;tag=1\r\nTo: <sip:b@x>\r\nCall-ID: c\r\nCSeq: 1 INVITE\r\n\r\n"));
    /* comma inside a quoted display name is fine */
    TEST_ASSERT_EQUAL(SIP_PARSE_OK,
        parse("INVITE sip:b@x SIP/2.0\r\nVia: SIP/2.0/UDP h;branch=z9hG4bK1\r\n"
              "From: \"Bell, Alexander\" <sip:a.g.bell@example.com>;tag=43\r\n"
              "To: caller<sip:b@x>\r\nCall-ID: c\r\nCSeq: 1 INVITE\r\n\r\n"));
    assert_str("Bell, Alexander", m.from.display);
    assert_str("caller", m.to.display); /* "lwsdisp" */
    /* missing '>' */
    TEST_ASSERT_EQUAL(SIP_PARSE_ERR,
        parse("INVITE sip:b@x SIP/2.0\r\nVia: SIP/2.0/UDP h;branch=z9hG4bK1\r\n"
              "From: <sip:a@x;tag=1\r\nTo: <sip:b@x>\r\nCall-ID: c\r\nCSeq: 1 INVITE\r\n\r\n"));
    /* empty tag */
    TEST_ASSERT_EQUAL(SIP_PARSE_ERR,
        parse("INVITE sip:b@x SIP/2.0\r\nVia: SIP/2.0/UDP h;branch=z9hG4bK1\r\n"
              "From: <sip:a@x>;tag=\r\nTo: <sip:b@x>\r\nCall-ID: c\r\nCSeq: 1 INVITE\r\n\r\n"));
    /* header without colon */
    TEST_ASSERT_EQUAL(SIP_PARSE_ERR, parse("INVITE sip:b@x SIP/2.0\r\n" BASIC_HDRS "NoColonHere\r\n\r\n"));
    /* Call-ID with whitespace */
    TEST_ASSERT_EQUAL(SIP_PARSE_ERR,
        parse("INVITE sip:b@x SIP/2.0\r\nVia: SIP/2.0/UDP h;branch=z9hG4bK1\r\n"
              "From: <sip:a@x>;tag=1\r\nTo: <sip:b@x>\r\nCall-ID: a b\r\nCSeq: 1 INVITE\r\n\r\n"));
}

static void test_nul_rejected(void)
{
    char tmp[256];
    int n = snprintf(tmp, sizeof(tmp), "INVITE sip:b@x SIP/2.0\r\n" BASIC_HDRS "X: a\r\n\r\n");
    tmp[n - 6] = '\0'; /* the 'a' */
    memcpy(buf, tmp, (size_t)n);
    TEST_ASSERT_EQUAL(SIP_PARSE_ERR, sip_msg_parse(&m, buf, (size_t)n));
    /* bare CR inside a header */
    TEST_ASSERT_EQUAL(SIP_PARSE_ERR, parse("INVITE sip:b@x SIP/2.0\r\n" BASIC_HDRS "X: a\rb\r\n\r\n"));
}

static void test_leading_crlf_skipped(void)
{
    TEST_ASSERT_EQUAL(SIP_PARSE_OK, parse("\r\n\r\nBYE sip:b@x SIP/2.0\r\nVia: SIP/2.0/UDP h;branch=z9hG4bK1\r\n"
                                          "From: <sip:a@x>;tag=1\r\nTo: <sip:b@x>;tag=2\r\nCall-ID: c\r\n"
                                          "CSeq: 2 BYE\r\n\r\n"));
    TEST_ASSERT_EQUAL(SIP_METHOD_BYE, m.method);
}

static void test_uri_parse(void)
{
    sip_uri_t u;
    TEST_ASSERT_EQUAL(0, sip_uri_parse(sip_str_c("sip:1001@10.0.0.2:5062;transport=udp;lr?subject=x"), &u));
    assert_str("1001", u.user);
    assert_str("10.0.0.2", u.host);
    TEST_ASSERT_EQUAL(5062, u.port);
    TEST_ASSERT_TRUE(u.lr);
    assert_str("subject=x", u.headers);
    TEST_ASSERT_EQUAL(0, sip_uri_parse(sip_str_c("sips:host.example.com"), &u));
    TEST_ASSERT_EQUAL(0, u.user.len);
    TEST_ASSERT_EQUAL(0, u.port);
    TEST_ASSERT_FALSE(u.lr);
    TEST_ASSERT_EQUAL(0, sip_uri_parse(sip_str_c("sip:user:pw@[2001:db8::1]:5060"), &u));
    assert_str("user", u.user);
    assert_str("[2001:db8::1]", u.host);
    TEST_ASSERT_EQUAL(0, sip_uri_parse(sip_str_c("sip:user;par=u%40example.net@example.com"), &u));
    assert_str("user;par=u%40example.net", u.user); /* "semiuri" */
    TEST_ASSERT_EQUAL(-1, sip_uri_parse(sip_str_c("tel:+1234"), &u));
    TEST_ASSERT_EQUAL(-1, sip_uri_parse(sip_str_c("sip:"), &u));
    TEST_ASSERT_EQUAL(-1, sip_uri_parse(sip_str_c("sip:a@h:99999"), &u));
    TEST_ASSERT_EQUAL(-1, sip_uri_parse(sip_str_c("sip:a@h:"), &u));
    TEST_ASSERT_EQUAL(-1, sip_uri_parse(sip_str_c("sip:a@h x"), &u));
    TEST_ASSERT_EQUAL(-1, sip_uri_parse(sip_str_c("sip:@h"), &u));
    TEST_ASSERT_EQUAL(-1, sip_uri_parse(sip_str_c("sip:a@[::1"), &u));
}

static void test_nameaddr(void)
{
    sip_nameaddr_t na;
    TEST_ASSERT_EQUAL(0, sip_nameaddr_parse(sip_str_c("*"), &na, true));
    TEST_ASSERT_TRUE(na.star);
    TEST_ASSERT_EQUAL(-1, sip_nameaddr_parse(sip_str_c("*"), &na, false));
    TEST_ASSERT_EQUAL(0, sip_nameaddr_parse(sip_str_c("sip:a@b;tag=x;expires=5"), &na, false));
    assert_str("sip:a@b", na.uri); /* addr-spec: params are header params */
    assert_str("x", na.tag);
    TEST_ASSERT_EQUAL(5, na.expires);
    TEST_ASSERT_EQUAL(0, sip_nameaddr_parse(sip_str_c("John Doe <sip:a@b;lr>;tag=y"), &na, false));
    assert_str("John Doe", na.display);
    assert_str("sip:a@b;lr", na.uri);
    TEST_ASSERT_EQUAL(-1, sip_nameaddr_parse(sip_str_c("John (x) <sip:a@b>"), &na, false));
    TEST_ASSERT_EQUAL(-1, sip_nameaddr_parse(sip_str_c("<>"), &na, false));
    TEST_ASSERT_EQUAL(-1, sip_nameaddr_parse(sip_str_c("<sip:a@b>;expires=x"), &na, false));
    char out[64];
    TEST_ASSERT_EQUAL(0, sip_nameaddr_parse(sip_str_c("\"A \\\"B\\\"\" <sip:a@b>;tag=1;foo"), &na, false));
    TEST_ASSERT_TRUE(sip_nameaddr_format(out, sizeof(out), &na) > 0);
    TEST_ASSERT_EQUAL_STRING("\"A \\\"B\\\"\" <sip:a@b>", out);
}

static void test_param_find_and_list(void)
{
    sip_str_t v;
    sip_str_t params = sip_str_c(";branch=z9hG4bK1 ; rport ;received=\"1.2.3.4\"");
    TEST_ASSERT_TRUE(sip_param_find(params, "BRANCH", &v));
    assert_str("z9hG4bK1", v);
    TEST_ASSERT_TRUE(sip_param_find(params, "rport", &v));
    TEST_ASSERT_EQUAL(0, v.len);
    TEST_ASSERT_TRUE(sip_param_find(params, "received", &v));
    assert_str("1.2.3.4", v);
    TEST_ASSERT_FALSE(sip_param_find(params, "maddr", &v));

    sip_str_t rest = sip_str_c(" a , \"b,c\" <d,e> ,, f");
    sip_str_t item;
    bool err = false;
    const char *exp[] = { "a", "\"b,c\" <d,e>", "f" };
    int i = 0;
    while (sip_list_next(&rest, &item, &err)) {
        TEST_ASSERT_TRUE(i < 3);
        assert_str(exp[i++], item);
    }
    TEST_ASSERT_EQUAL(3, i);
    TEST_ASSERT_FALSE(err);
    rest = sip_str_c("<a, b");
    while (sip_list_next(&rest, &item, &err)) {
    }
    TEST_ASSERT_TRUE(err);
}

static void test_writer(void)
{
    char out[8];
    sip_wbuf_t w;
    sip_w_init(&w, out, sizeof(out));
    sip_w_cstr(&w, "abc");
    sip_w_fmt(&w, "%d", 1234);
    TEST_ASSERT_FALSE(w.overflow);
    TEST_ASSERT_EQUAL_STRING("abc1234", out);
    sip_w_cstr(&w, "x");
    TEST_ASSERT_TRUE(w.overflow);
    TEST_ASSERT_EQUAL_STRING("abc1234", out);
    sip_w_init(&w, out, sizeof(out));
    sip_w_fmt(&w, "%s", "123456789");
    TEST_ASSERT_TRUE(w.overflow);
    TEST_ASSERT_EQUAL(0, w.len);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_basic_request);
    RUN_TEST(test_basic_response);
    RUN_TEST(test_rfc4475_wsinv);
    RUN_TEST(test_rfc4475_intmeth);
    RUN_TEST(test_rfc4475_esc01);
    RUN_TEST(test_escnull);
    RUN_TEST(test_compact_forms);
    RUN_TEST(test_compact_forms_ok);
    RUN_TEST(test_duplicate_content_length_rejected);
    RUN_TEST(test_multiple_via_and_contact_lists);
    RUN_TEST(test_too_many_vias_rejected);
    RUN_TEST(test_unknown_headers_and_bare_lf);
    RUN_TEST(test_content_length_handling);
    RUN_TEST(test_oversized_rejected);
    RUN_TEST(test_missing_mandatory_rejected);
    RUN_TEST(test_duplicate_single_headers_rejected);
    RUN_TEST(test_bad_start_lines);
    RUN_TEST(test_scalar_overflow_rejected);
    RUN_TEST(test_quoting_and_separators);
    RUN_TEST(test_nul_rejected);
    RUN_TEST(test_leading_crlf_skipped);
    RUN_TEST(test_uri_parse);
    RUN_TEST(test_nameaddr);
    RUN_TEST(test_param_find_and_list);
    RUN_TEST(test_writer);
    return UNITY_END();
}
