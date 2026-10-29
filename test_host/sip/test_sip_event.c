/* Event package parsers: Subscription-State / Event headers, dialog-info+xml
 * (RFC 4235) and simple-message-summary (RFC 3842). */

#include <stdio.h>
#include <string.h>

#include "sip_event.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static int parse(const char *xml, sip_dlginfo_t *di)
{
    return sip_dlginfo_parse(xml, strlen(xml), di);
}

/* ---- Asterisk 20 bodies (res_pjsip_dialog_info_body_generator), captured
 * from test_host/sip/integration.sh (VERBOSE=1): idle, ringing, in use ---- */

static const char *AST_IDLE =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<dialog-info xmlns=\"urn:ietf:params:xml:ns:dialog-info\" version=\"0\" state=\"full\" "
    "entity=\"sip:1002@127.0.0.1:5080\">\n"
    " <dialog id=\"1002\">\n"
    "  <state>terminated</state>\n"
    " </dialog>\n"
    "</dialog-info>\n";

static const char *AST_RINGING =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<dialog-info xmlns=\"urn:ietf:params:xml:ns:dialog-info\" version=\"1\" state=\"full\" "
    "entity=\"sip:1002@127.0.0.1:5080\">\n"
    " <dialog id=\"1002\" direction=\"recipient\">\n"
    "  <remote>\n"
    "   <identity>sip:invalid@127.0.0.1</identity>\n"
    "   <target uri=\"sip:invalid@127.0.0.1\" />\n"
    "  </remote>\n"
    "  <local>\n"
    "   <identity>sip:invalid@127.0.0.1</identity>\n"
    "   <target uri=\"sip:1002@127.0.0.1:5080\" />\n"
    "  </local>\n"
    "  <state>early</state>\n"
    " </dialog>\n"
    "</dialog-info>\n";

static const char *AST_BUSY =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<dialog-info xmlns=\"urn:ietf:params:xml:ns:dialog-info\" version=\"2\" state=\"full\" "
    "entity=\"sip:1002@127.0.0.1:5080\">\n"
    " <dialog id=\"1002\">\n"
    "  <state>confirmed</state>\n"
    " </dialog>\n"
    "</dialog-info>\n";

/* Asterisk style for a held call (local target with +sip.rendering=no) */
static const char *AST_ONHOLD =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n"
    "<dialog-info xmlns=\"urn:ietf:params:xml:ns:dialog-info\" version=\"3\" state=\"full\" "
    "entity=\"sip:1002@127.0.0.1:5080\">\r\n"
    " <dialog id=\"1002\">\r\n"
    "  <state>confirmed</state>\r\n"
    "  <local>\r\n"
    "   <target uri=\"sip:1002@127.0.0.1:5080\">\r\n"
    "    <param pname=\"+sip.rendering\" pvalue=\"no\"/>\r\n"
    "   </target>\r\n"
    "  </local>\r\n"
    " </dialog>\r\n"
    "</dialog-info>\r\n";

static void test_asterisk_samples(void)
{
    sip_dlginfo_t di;
    TEST_ASSERT_EQUAL(0, parse(AST_IDLE, &di));
    TEST_ASSERT_EQUAL_UINT32(0, di.version);
    TEST_ASSERT_TRUE(di.full);
    TEST_ASSERT_EQUAL(1, di.n);
    TEST_ASSERT_EQUAL_STRING("1002", di.d[0].id);
    TEST_ASSERT_EQUAL(SIP_DLG_TERMINATED, di.d[0].state);
    TEST_ASSERT_EQUAL(SIP_DLGDIR_NONE, di.d[0].dir);

    TEST_ASSERT_EQUAL(0, parse(AST_RINGING, &di));
    TEST_ASSERT_EQUAL_UINT32(1, di.version);
    TEST_ASSERT_EQUAL(SIP_DLG_EARLY, di.d[0].state);
    TEST_ASSERT_EQUAL(SIP_DLGDIR_RECIPIENT, di.d[0].dir);

    TEST_ASSERT_EQUAL(0, parse(AST_BUSY, &di));
    TEST_ASSERT_EQUAL_UINT32(2, di.version);
    TEST_ASSERT_EQUAL(SIP_DLG_CONFIRMED, di.d[0].state);

    TEST_ASSERT_EQUAL(0, parse(AST_ONHOLD, &di));
    TEST_ASSERT_EQUAL_UINT32(3, di.version);
    TEST_ASSERT_EQUAL(1, di.n);
    TEST_ASSERT_EQUAL(SIP_DLG_CONFIRMED, di.d[0].state);
}

static void test_rfc4235_example_and_prefixes(void)
{
    /* RFC 4235 §6.1-style document with identities, remote target, comments, PI */
    const char *rfc =
        "<?xml version=\"1.0\"?>\n"
        "<!-- comment before the root -->\n"
        "<dialog-info xmlns=\"urn:ietf:params:xml:ns:dialog-info\"\n"
        "             version='12' state='partial'\n"
        "             entity=\"sip:alice@example.com\">\n"
        "  <dialog id=\"as7d900as8\" call-id=\"a84b4c76e66710\" local-tag=\"1928301774\"\n"
        "          remote-tag=\"456887766\" direction=\"initiator\">\n"
        "    <state event=\"remote-bye\" code=\"200\">terminated</state>\n"
        "    <duration>274</duration>\n"
        "    <local><identity display=\"Alice &amp; Co\">sip:alice@example.com</identity></local>\n"
        "    <remote><identity>sip:bob@example.org</identity>\n"
        "            <target uri=\"sip:bob@pc33.example.org\"/></remote>\n"
        "  </dialog>\n"
        "  <dialog id=\"x2\"><state>  confirmed\n </state><!-- c --></dialog>\n"
        "  <dialog id=\"x3\"/>\n"
        "</dialog-info>\n";
    sip_dlginfo_t di;
    TEST_ASSERT_EQUAL(0, parse(rfc, &di));
    TEST_ASSERT_FALSE(di.full);
    TEST_ASSERT_EQUAL_UINT32(12, di.version);
    TEST_ASSERT_EQUAL(3, di.n);
    TEST_ASSERT_EQUAL(SIP_DLG_TERMINATED, di.d[0].state);
    TEST_ASSERT_EQUAL(SIP_DLGDIR_INITIATOR, di.d[0].dir);
    TEST_ASSERT_EQUAL(SIP_DLG_CONFIRMED, di.d[1].state);
    TEST_ASSERT_EQUAL_STRING("x3", di.d[2].id);
    TEST_ASSERT_EQUAL(SIP_DLG_UNKNOWN, di.d[2].state);

    const char *pfx =
        "<d:dialog-info xmlns:d=\"urn:ietf:params:xml:ns:dialog-info\" version=\"2\" state=\"full\" entity=\"x\">"
        "<d:dialog id=\"1\" direction=\"recipient\"><d:state>early</d:state></d:dialog></d:dialog-info>";
    TEST_ASSERT_EQUAL(0, parse(pfx, &di));
    TEST_ASSERT_EQUAL(1, di.n);
    TEST_ASSERT_EQUAL(SIP_DLG_EARLY, di.d[0].state);

    /* no dialogs: idle */
    TEST_ASSERT_EQUAL(0, parse("<dialog-info version=\"5\" state=\"full\" entity=\"x\"/>", &di));
    TEST_ASSERT_EQUAL(0, di.n);
    TEST_ASSERT_EQUAL_UINT32(5, di.version);
    /* UTF-8 BOM */
    TEST_ASSERT_EQUAL(0, parse("\xEF\xBB\xBF<dialog-info version=\"1\" state=\"full\"></dialog-info>", &di));
    /* <state> elsewhere (inside <local>) is not the dialog state */
    TEST_ASSERT_EQUAL(0, parse("<dialog-info version=\"1\" state=\"full\"><dialog id=\"a\"><local><state>"
                               "early</state></local></dialog></dialog-info>",
                               &di));
    TEST_ASSERT_EQUAL(SIP_DLG_UNKNOWN, di.d[0].state);
}

static void test_many_dialogs_and_long_id(void)
{
    char doc[3000];
    size_t n = (size_t)snprintf(doc, sizeof(doc), "<dialog-info version=\"1\" state=\"full\">");
    for (int i = 0; i < 11; i++) {
        n += (size_t)snprintf(doc + n, sizeof(doc) - n, "<dialog id=\"d%d\"><state>confirmed</state></dialog>", i);
    }
    n += (size_t)snprintf(doc + n, sizeof(doc) - n,
                          "<dialog id=\"0123456789012345678901234567890123456789012345678901234567890\">"
                          "<state>early</state></dialog></dialog-info>");
    sip_dlginfo_t di;
    TEST_ASSERT_EQUAL(0, sip_dlginfo_parse(doc, n, &di));
    TEST_ASSERT_EQUAL(SIP_DLGINFO_MAX_DIALOGS, di.n);
    TEST_ASSERT_EQUAL(12, di.n_total);
    TEST_ASSERT_EQUAL_STRING("d7", di.d[7].id);
}

static void test_malformed_rejected(void)
{
    static const char *const bad[] = {
        "",
        "   ",
        "<dialog-info>",                                           /* unclosed root */
        "<dialog-info></dialog>",                                  /* mismatched */
        "<dialog-info><dialog></dialog-info>",                     /* unclosed child */
        "<dialog-info></dialog-info><dialog-info></dialog-info>",  /* two roots */
        "<presence></presence>",                                   /* wrong document */
        "junk<dialog-info></dialog-info>",                         /* text outside root */
        "<dialog-info></dialog-info>junk",
        "<dialog-info version=\"1></dialog-info>",                 /* unterminated value */
        "<dialog-info version=1></dialog-info>",                   /* unquoted value */
        "<dialog-info version></dialog-info>",                     /* no value */
        "<dialog-info version=\"x\"></dialog-info>",               /* bad version */
        "<dialog-info version=\"99999999999\"></dialog-info>",     /* overflow */
        "<dialog-info state=\"most\"></dialog-info>",              /* bad state attr */
        "<dialog-info a=\"<\"></dialog-info>",                     /* '<' in a value */
        "<dialog-info a=\"1\"b=\"2\"></dialog-info>",              /* no space between attrs */
        "<!DOCTYPE x [<!ENTITY a \"b\">]><dialog-info></dialog-info>",
        "<dialog-info><![CDATA[<dialog>]]></dialog-info>",
        "<dialog-info><!-- unterminated </dialog-info>",
        "<?xml version=\"1.0\"<dialog-info></dialog-info>",        /* unterminated PI */
        "<dialog-info></ dialog-info>",
        "<dialog-info><></dialog-info>",
        "<dialog-info/ >",
        "<dialog-info></dialog-info",
        "<",
    };
    sip_dlginfo_t di;
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        char msg[96];
        snprintf(msg, sizeof(msg), "case %u: %.60s", (unsigned)i, bad[i]);
        TEST_ASSERT_EQUAL_MESSAGE(-1, parse(bad[i], &di), msg);
    }
    /* NUL inside */
    const char nul[] = "<dialog-info>\0</dialog-info>";
    TEST_ASSERT_EQUAL(-1, sip_dlginfo_parse(nul, sizeof(nul) - 1, &di));
    TEST_ASSERT_EQUAL(-1, sip_dlginfo_parse(NULL, 10, &di));
}

static void test_truncated_rejected(void)
{
    /* every proper prefix of a valid document that cuts into the root fails */
    const char *docs[] = { AST_IDLE, AST_RINGING, AST_BUSY, AST_ONHOLD };
    sip_dlginfo_t di;
    for (size_t k = 0; k < 4; k++) {
        const char *end = strstr(docs[k], "</dialog-info>");
        size_t full = (size_t)(end - docs[k]) + strlen("</dialog-info>");
        for (size_t n = 0; n < full; n++) {
            char msg[48];
            snprintf(msg, sizeof(msg), "doc %u cut at %u", (unsigned)k, (unsigned)n);
            TEST_ASSERT_EQUAL_MESSAGE(-1, sip_dlginfo_parse(docs[k], n, &di), msg);
        }
        TEST_ASSERT_EQUAL(0, sip_dlginfo_parse(docs[k], full, &di));
        TEST_ASSERT_EQUAL(0, sip_dlginfo_parse(docs[k], strlen(docs[k]), &di));
    }
}

static void test_oversized_and_deep(void)
{
    static char big[SIP_DLGINFO_MAX_LEN + 64];
    sip_dlginfo_t di;
    size_t n = (size_t)snprintf(big, sizeof(big), "<dialog-info>");
    while (n < SIP_DLGINFO_MAX_LEN + 10) {
        big[n++] = ' ';
    }
    n += (size_t)snprintf(big + n, sizeof(big) - n, "</dialog-info>");
    TEST_ASSERT_EQUAL(-1, sip_dlginfo_parse(big, n, &di));

    /* nesting: 16 levels fine, 17 rejected */
    char deep[1024];
    for (int levels = 16; levels <= 17; levels++) {
        n = (size_t)snprintf(deep, sizeof(deep), "<dialog-info>");
        for (int i = 1; i < levels; i++) {
            n += (size_t)snprintf(deep + n, sizeof(deep) - n, "<a>");
        }
        for (int i = 1; i < levels; i++) {
            n += (size_t)snprintf(deep + n, sizeof(deep) - n, "</a>");
        }
        n += (size_t)snprintf(deep + n, sizeof(deep) - n, "</dialog-info>");
        TEST_ASSERT_EQUAL(levels == 16 ? 0 : -1, sip_dlginfo_parse(deep, n, &di));
    }
}

/* ---- message-summary ---- */

static void test_msgsum(void)
{
    sip_msgsum_t ms;
    const char *a = "Messages-Waiting: yes\r\nMessage-Account: sip:1001@127.0.0.1\r\nVoice-Message: 2/8 (0/2)\r\n";
    TEST_ASSERT_EQUAL(0, sip_msgsum_parse(a, strlen(a), &ms));
    TEST_ASSERT_TRUE(ms.waiting);
    TEST_ASSERT_TRUE(ms.have_voice);
    TEST_ASSERT_EQUAL_UINT32(2, ms.new_msgs);
    TEST_ASSERT_EQUAL_UINT32(8, ms.old_msgs);
    TEST_ASSERT_EQUAL_UINT32(0, ms.new_urgent);
    TEST_ASSERT_EQUAL_UINT32(2, ms.old_urgent);

    const char *b = "messages-waiting:NO\nvoice-message:0/0\n";
    TEST_ASSERT_EQUAL(0, sip_msgsum_parse(b, strlen(b), &ms));
    TEST_ASSERT_FALSE(ms.waiting);
    TEST_ASSERT_TRUE(ms.have_voice);

    /* Asterisk 20 (res_pjsip_mwi_body_generator), captured by integration.sh */
    const char *c = "Messages-Waiting: yes\r\nVoice-Message: 1/0 (0/0)\r\n";
    TEST_ASSERT_EQUAL(0, sip_msgsum_parse(c, strlen(c), &ms));
    TEST_ASSERT_EQUAL_UINT32(1, ms.new_msgs);

    /* RFC 3842 example with other classes and message headers */
    const char *d = "Messages-Waiting: yes\r\nMessage-Account: sip:alice@vmail.example.com\r\n"
                    "Voice-Message: 4/8 (1/2)\r\nFax-Message: 1/1\r\n\r\n"
                    "To: <alice@atlanta.example.com>\r\nFrom: <bob@biloxi.example.com>\r\n";
    TEST_ASSERT_EQUAL(0, sip_msgsum_parse(d, strlen(d), &ms));
    TEST_ASSERT_EQUAL_UINT32(4, ms.new_msgs);
    TEST_ASSERT_EQUAL_UINT32(1, ms.new_urgent);

    /* waiting without counts */
    TEST_ASSERT_EQUAL(0, sip_msgsum_parse("Messages-Waiting: yes", 21, &ms));
    TEST_ASSERT_TRUE(ms.waiting);
    TEST_ASSERT_FALSE(ms.have_voice);
    /* malformed counts are ignored, huge ones capped */
    const char *e = "Messages-Waiting: yes\r\nVoice-Message: x/2\r\n";
    TEST_ASSERT_EQUAL(0, sip_msgsum_parse(e, strlen(e), &ms));
    TEST_ASSERT_FALSE(ms.have_voice);
    const char *g = "Messages-Waiting: yes\r\nVoice-Message: 99999999999999/3 (1/\r\n";
    TEST_ASSERT_EQUAL(0, sip_msgsum_parse(g, strlen(g), &ms));
    TEST_ASSERT_FALSE(ms.have_voice);
    const char *h = "Messages-Waiting: yes\r\nVoice-Message: 99999999999999/3\r\n";
    TEST_ASSERT_EQUAL(0, sip_msgsum_parse(h, strlen(h), &ms));
    TEST_ASSERT_EQUAL_UINT32(999999, ms.new_msgs);
}

static void test_msgsum_bad(void)
{
    sip_msgsum_t ms;
    static const char *const bad[] = {
        "",
        "Voice-Message: 1/0\r\n",                 /* no Messages-Waiting */
        "Messages-Waiting: maybe\r\n",
        "Messages-Waiting yes\r\n",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        TEST_ASSERT_EQUAL(-1, sip_msgsum_parse(bad[i], strlen(bad[i]), &ms));
    }
    const char nul[] = "Messages-Waiting: yes\0\r\n";
    TEST_ASSERT_EQUAL(-1, sip_msgsum_parse(nul, sizeof(nul) - 1, &ms));
    static char big[5000];
    memset(big, 'a', sizeof(big));
    TEST_ASSERT_EQUAL(-1, sip_msgsum_parse(big, sizeof(big), &ms));
}

/* ---- headers ---- */

static void test_sub_state(void)
{
    sip_sub_state_t ss;
    TEST_ASSERT_EQUAL(0, sip_sub_state_parse(sip_str_c("active;expires=600"), &ss));
    TEST_ASSERT_EQUAL(SIP_SUBST_ACTIVE, ss.state);
    TEST_ASSERT_EQUAL(600, ss.expires);
    TEST_ASSERT_EQUAL(-1, ss.retry_after);
    TEST_ASSERT_EQUAL(SIP_SUBR_NONE, ss.reason);
    TEST_ASSERT_EQUAL(0, sip_sub_state_parse(sip_str_c(" terminated ; reason=noresource ;retry-after=30"), &ss));
    TEST_ASSERT_EQUAL(SIP_SUBST_TERMINATED, ss.state);
    TEST_ASSERT_EQUAL(SIP_SUBR_NORESOURCE, ss.reason);
    TEST_ASSERT_EQUAL(30, ss.retry_after);
    TEST_ASSERT_EQUAL(0, sip_sub_state_parse(sip_str_c("Pending"), &ss));
    TEST_ASSERT_EQUAL(SIP_SUBST_PENDING, ss.state);
    TEST_ASSERT_EQUAL(0, sip_sub_state_parse(sip_str_c("terminated;reason=whatever"), &ss));
    TEST_ASSERT_EQUAL(SIP_SUBR_OTHER, ss.reason);
    TEST_ASSERT_EQUAL(0, sip_sub_state_parse(sip_str_c("terminated;reason=deactivated"), &ss));
    TEST_ASSERT_EQUAL(SIP_SUBR_DEACTIVATED, ss.reason);
    TEST_ASSERT_EQUAL(0, sip_sub_state_parse(sip_str_c("active;expires=abc"), &ss));
    TEST_ASSERT_EQUAL(-1, ss.expires);
    TEST_ASSERT_EQUAL(0, sip_sub_state_parse(sip_str_c("frozen"), &ss));
    TEST_ASSERT_EQUAL(SIP_SUBST_OTHER, ss.state);
    TEST_ASSERT_EQUAL(-1, sip_sub_state_parse(sip_str_c(""), &ss));
    TEST_ASSERT_EQUAL(-1, sip_sub_state_parse(sip_str_c(";expires=1"), &ss));
}

static void test_event_header(void)
{
    sip_str_t pkg, id;
    TEST_ASSERT_EQUAL(0, sip_event_parse(sip_str_c("dialog"), &pkg, &id));
    TEST_ASSERT_TRUE(sip_str_eq(pkg, sip_str_c("dialog")));
    TEST_ASSERT_NULL(id.p);
    TEST_ASSERT_EQUAL(0, sip_event_parse(sip_str_c(" message-summary ;id=7"), &pkg, &id));
    TEST_ASSERT_TRUE(sip_str_eq(pkg, sip_str_c("message-summary")));
    TEST_ASSERT_TRUE(sip_str_eq(id, sip_str_c("7")));
    TEST_ASSERT_EQUAL(-1, sip_event_parse(sip_str_c(""), &pkg, &id));
    TEST_ASSERT_EQUAL(-1, sip_event_parse(sip_str_c("dia log"), &pkg, &id));
    TEST_ASSERT_TRUE(sip_content_type_is(sip_str_c("Application/Dialog-Info+XML; charset=utf-8"),
                                         "application/dialog-info+xml"));
    TEST_ASSERT_FALSE(sip_content_type_is(SIP_STR_NULL, "application/sdp"));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_asterisk_samples);
    RUN_TEST(test_rfc4235_example_and_prefixes);
    RUN_TEST(test_many_dialogs_and_long_id);
    RUN_TEST(test_malformed_rejected);
    RUN_TEST(test_truncated_rejected);
    RUN_TEST(test_oversized_and_deep);
    RUN_TEST(test_msgsum);
    RUN_TEST(test_msgsum_bad);
    RUN_TEST(test_sub_state);
    RUN_TEST(test_event_header);
    return UNITY_END();
}
