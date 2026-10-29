/* Host tests for components/directory/dir_core.c. */

#include <stdlib.h>
#include <string.h>

#include "dir_core.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static dir_contact_t s_store[DIR_CONTACTS_MAX];
static dir_ctab_t tab(void)
{
    memset(s_store, 0, sizeof(s_store));
    return (dir_ctab_t){ .v = s_store, .n = 0, .cap = DIR_CONTACTS_MAX };
}

static void norm_eq(const char *in, const char *want)
{
    char out[DIR_NUMBER_MAX];
    dir_core_normalize(in, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING(want, out);
}

static void test_normalize(void)
{
    norm_eq("1002", "1002");
    norm_eq(" +1 (555) 123-4567 ", "+15551234567");
    norm_eq("sip:1002@192.168.1.1:5060", "1002");
    norm_eq("SIP:1002@host;transport=udp", "1002");
    norm_eq("\"Bob\" <sip:1002@host>;tag=x", "1002");
    norm_eq("<sips:+4930.123@x>", "+4930123");
    norm_eq("tel:+49-30-123", "+4930123");
    norm_eq("sip:Alice.B@host", "alice.b");
    norm_eq("*97", "*97");
    norm_eq("12+34", "1234");     /* '+' only leading */
    norm_eq("", "");
    norm_eq(NULL, "");
    norm_eq("   ", "");
    char tiny[4];
    dir_core_normalize("123456789", tiny, sizeof(tiny));
    TEST_ASSERT_EQUAL_STRING("123", tiny);
}

static void test_clean_name(void)
{
    char o[DIR_NAME_MAX];
    dir_core_clean_name("  Bob \t  Smith\n", o, sizeof(o));
    TEST_ASSERT_EQUAL_STRING("Bob Smith", o);
    /* 2-byte chars cut mid-sequence: keep whole characters only */
    dir_core_clean_name("aaaa\xc3\xa4\xc3\xa4", o, 7);   /* a a a a + ae + (cut) */
    TEST_ASSERT_EQUAL_STRING("aaaa\xc3\xa4", o);
    dir_core_clean_name(NULL, o, sizeof(o));
    TEST_ASSERT_EQUAL_STRING("", o);
}

static void test_contacts_sorted_update_delete(void)
{
    dir_ctab_t t = tab();
    TEST_ASSERT_EQUAL_INT(DIR_C_ADDED, dir_ctab_set(&t, "1003", "carol"));
    TEST_ASSERT_EQUAL_INT(DIR_C_ADDED, dir_ctab_set(&t, "1001", "Alice"));
    TEST_ASSERT_EQUAL_INT(DIR_C_ADDED, dir_ctab_set(&t, "sip:1002@x", "Bob"));
    TEST_ASSERT_EQUAL_INT(DIR_C_ADDED, dir_ctab_set(&t, "1004", "alice"));  /* same name, number tiebreak */
    TEST_ASSERT_EQUAL(4, t.n);
    TEST_ASSERT_EQUAL_STRING("1001", t.v[0].number);
    TEST_ASSERT_EQUAL_STRING("1004", t.v[1].number);
    TEST_ASSERT_EQUAL_STRING("Bob", t.v[2].name);
    TEST_ASSERT_EQUAL_STRING("carol", t.v[3].name);
    /* update by another spelling of the same number, re-sorts */
    TEST_ASSERT_EQUAL_INT(DIR_C_UPDATED, dir_ctab_set(&t, "1-0-0-2", "Zed"));
    TEST_ASSERT_EQUAL(4, t.n);
    TEST_ASSERT_EQUAL_STRING("Zed", t.v[3].name);
    TEST_ASSERT_EQUAL_INT(DIR_C_INVALID, dir_ctab_set(&t, "", "x"));
    TEST_ASSERT_EQUAL_INT(DIR_C_INVALID, dir_ctab_set(&t, "12", "  "));
    TEST_ASSERT_TRUE(dir_ctab_del(&t, "tel:1003"));
    TEST_ASSERT_FALSE(dir_ctab_del(&t, "1003"));
    TEST_ASSERT_EQUAL(3, t.n);
    TEST_ASSERT_EQUAL_INT(-1, dir_ctab_find(&t, "1003"));
    TEST_ASSERT_EQUAL_INT(2, dir_ctab_find(&t, "sip:1002@pbx"));
}

static void test_contacts_full(void)
{
    dir_ctab_t t = tab();
    char num[16], name[16];
    for (int i = 0; i < DIR_CONTACTS_MAX; i++) {
        snprintf(num, sizeof(num), "%d", 1000 + i);
        snprintf(name, sizeof(name), "n%d", i);
        TEST_ASSERT_EQUAL_INT(DIR_C_ADDED, dir_ctab_set(&t, num, name));
    }
    TEST_ASSERT_EQUAL_INT(DIR_C_FULL, dir_ctab_set(&t, "9999", "x"));
    TEST_ASSERT_EQUAL_INT(DIR_C_UPDATED, dir_ctab_set(&t, "1000", "renamed"));   /* still fine */
    TEST_ASSERT_EQUAL(DIR_CONTACTS_MAX, t.n);
}

static void test_search(void)
{
    dir_ctab_t t = tab();
    dir_ctab_set(&t, "1001", "Alice");
    dir_ctab_set(&t, "1002", "Bob");
    dir_ctab_set(&t, "2001", "Charlie Alison");
    dir_ctab_set(&t, "+49301002", "Dora");
    size_t idx[8];
    size_t n = dir_ctab_search(&t, "ali", idx, 8);
    TEST_ASSERT_EQUAL(2, n);
    TEST_ASSERT_EQUAL_STRING("Alice", t.v[idx[0]].name);            /* prefix first */
    TEST_ASSERT_EQUAL_STRING("Charlie Alison", t.v[idx[1]].name);   /* substring after */
    n = dir_ctab_search(&t, "100", idx, 8);                         /* number prefix, then substring */
    TEST_ASSERT_EQUAL(3, n);
    TEST_ASSERT_EQUAL_STRING("1001", t.v[idx[0]].number);
    TEST_ASSERT_EQUAL_STRING("1002", t.v[idx[1]].number);
    TEST_ASSERT_EQUAL_STRING("+49301002", t.v[idx[2]].number);
    TEST_ASSERT_EQUAL(1, dir_ctab_search(&t, "2-0", idx, 8));       /* query is normalised too */
    TEST_ASSERT_EQUAL(0, dir_ctab_search(&t, "zzz", idx, 8));
    TEST_ASSERT_EQUAL(4, dir_ctab_search(&t, "", idx, 8));
    TEST_ASSERT_EQUAL(2, dir_ctab_search(&t, "", idx, 2));          /* max honoured */
}

static void parse_line_eq(const char *line, int rc, const char *num, const char *name)
{
    char n[DIR_NUMBER_MAX] = "", m[DIR_NAME_MAX] = "";
    TEST_ASSERT_EQUAL_INT_MESSAGE(rc, dir_csv_parse_line(line, n, sizeof(n), m, sizeof(m)), line);
    if (rc == 0) {
        TEST_ASSERT_EQUAL_STRING_MESSAGE(num, n, line);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(name, m, line);
    }
}

static void test_csv_lines(void)
{
    parse_line_eq("1002,Bob", 0, "1002", "Bob");
    parse_line_eq("  1002 , Bob Smith \r\n", 0, "1002", "Bob Smith");
    parse_line_eq("Bob,1002", 0, "1002", "Bob");                     /* swapped */
    parse_line_eq("\"Smith, Bob\",555-1234", 0, "5551234", "Smith, Bob");
    parse_line_eq("1002,\"Say \"\"hi\"\"\"", 0, "1002", "Say \"hi\"");
    parse_line_eq("1002;Bob", 0, "1002", "Bob");
    parse_line_eq("1002\tBob", 0, "1002", "Bob");
    parse_line_eq("1002,Bob, Jr", 0, "1002", "Bob, Jr");             /* name keeps commas */
    parse_line_eq("alice,Alice", 0, "alice", "Alice");               /* alphanumeric user */
    parse_line_eq("", 1, NULL, NULL);
    parse_line_eq("   \n", 1, NULL, NULL);
    parse_line_eq("# comment", 1, NULL, NULL);
    parse_line_eq("number,name", 1, NULL, NULL);
    parse_line_eq("1002", -1, NULL, NULL);
    parse_line_eq("1002,", -1, NULL, NULL);
    parse_line_eq(",Bob", -1, NULL, NULL);
    char longline[400];
    memset(longline, 'a', sizeof(longline) - 1);
    longline[sizeof(longline) - 1] = '\0';
    parse_line_eq(longline, -1, NULL, NULL);
}

static void test_csv_format_roundtrip(void)
{
    dir_ctab_t t = tab();
    dir_ctab_set(&t, "1001", "Alice");
    dir_ctab_set(&t, "1002", "Smith, Bob \"B\"");
    dir_ctab_set(&t, "+4930123", "D\xc3\xb6rte");
    char line[DIR_LINE_MAX];
    dir_csv_format_line(&t.v[0], line, sizeof(line));
    TEST_ASSERT_EQUAL_STRING("1001,Alice", line);
    size_t need = dir_csv_format_line(&t.v[2], line, sizeof(line));
    TEST_ASSERT_EQUAL_STRING("1002,\"Smith, Bob \"\"B\"\"\"", line);
    TEST_ASSERT_EQUAL(strlen(line), need);
    char small[5];
    need = dir_csv_format_line(&t.v[0], small, sizeof(small));    /* truncation: NUL-terminated, full length returned */
    TEST_ASSERT_EQUAL(10, need);
    TEST_ASSERT_EQUAL_STRING("1001", small);

    /* file round trip via tmpfile */
    FILE *f = tmpfile();
    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_EQUAL(0, dir_ctab_write(&t, f));
    long len = ftell(f);
    rewind(f);
    char *text = malloc((size_t)len + 1);
    TEST_ASSERT_EQUAL((size_t)len, fread(text, 1, (size_t)len, f));
    fclose(f);
    dir_contact_t other[DIR_CONTACTS_MAX];
    dir_ctab_t u = { .v = other, .n = 0, .cap = DIR_CONTACTS_MAX };
    size_t bad = 99;
    bool complete = false;
    TEST_ASSERT_EQUAL(3, dir_ctab_parse(&u, text, (size_t)len, &bad, &complete));
    TEST_ASSERT_EQUAL(0, bad);
    TEST_ASSERT_TRUE(complete);
    TEST_ASSERT_EQUAL(3, u.n);
    for (size_t i = 0; i < 3; i++) {
        TEST_ASSERT_EQUAL_STRING(t.v[i].number, u.v[i].number);
        TEST_ASSERT_EQUAL_STRING(t.v[i].name, u.v[i].name);
    }
    /* truncated file (power cut mid-write): parses what is there, not complete, no crash */
    u.n = 0;
    TEST_ASSERT_TRUE(dir_ctab_parse(&u, text, (size_t)len - 12, &bad, &complete) >= 1);
    TEST_ASSERT_FALSE(complete);
    free(text);
}

static void test_csv_garbage(void)
{
    dir_ctab_t t = tab();
    const char *g = "\x01\x02\xff\xfe,\n,,,\n\"unterminated,12\n1005,Ok\n\n\n#end";
    size_t bad = 0;
    bool complete = false;
    size_t n = dir_ctab_parse(&t, g, strlen(g), &bad, &complete);
    TEST_ASSERT_TRUE(n >= 1);
    TEST_ASSERT_TRUE(bad >= 1);
    TEST_ASSERT_TRUE(complete);
    TEST_ASSERT_TRUE(dir_ctab_find(&t, "1005") >= 0);
    /* random bytes */
    srand(7);
    for (int round = 0; round < 200; round++) {
        char buf[300];
        size_t len = (size_t)(rand() % (int)sizeof(buf));
        for (size_t i = 0; i < len; i++) {
            buf[i] = (char)(rand() % 5 == 0 ? '\n' : rand() % 5 == 0 ? ',' : rand());
        }
        t.n = 0;
        dir_ctab_parse(&t, buf, len, NULL, NULL);
        TEST_ASSERT_TRUE(t.n <= t.cap);
    }
}

static void test_speed(void)
{
    dir_speed_tab_t t;
    memset(&t, 0, sizeof(t));
    TEST_ASSERT_TRUE(dir_speed_set_tab(&t, 2, "sip:1002@x"));
    TEST_ASSERT_TRUE(dir_speed_set_tab(&t, 9, "+49 30 1"));
    TEST_ASSERT_FALSE(dir_speed_set_tab(&t, 1, "1"));
    TEST_ASSERT_FALSE(dir_speed_set_tab(&t, 10, "1"));
    TEST_ASSERT_FALSE(dir_speed_set_tab(&t, 0, "1"));
    FILE *f = tmpfile();
    TEST_ASSERT_EQUAL(0, dir_speed_write(&t, f));
    long len = ftell(f);
    rewind(f);
    char text[256];
    TEST_ASSERT_EQUAL((size_t)len, fread(text, 1, (size_t)len, f));
    fclose(f);
    dir_speed_tab_t u;
    memset(&u, 0, sizeof(u));
    bool complete = false;
    TEST_ASSERT_EQUAL(2, dir_speed_parse(&u, text, (size_t)len, &complete));
    TEST_ASSERT_TRUE(complete);
    TEST_ASSERT_EQUAL_STRING("1002", u.num[0]);
    TEST_ASSERT_EQUAL_STRING("+49301", u.num[7]);
    TEST_ASSERT_EQUAL_STRING("", u.num[3]);
    TEST_ASSERT_TRUE(dir_speed_set_tab(&t, 2, ""));         /* clear */
    TEST_ASSERT_EQUAL_STRING("", t.num[0]);
    const char *bad = "1,5\n0,7\nx,y\n2\n5,\n";
    memset(&u, 0, sizeof(u));
    TEST_ASSERT_EQUAL(1, dir_speed_parse(&u, bad, strlen(bad), &complete));   /* "5," clears: counted, harmless */
}

static dir_call_t call(uint8_t dir, uint8_t res, const char *num, time_t t)
{
    dir_call_t c;
    memset(&c, 0, sizeof(c));
    c.direction = dir;
    c.result = res;
    c.start = t;
    c.seen = !(dir == DIR_CALL_IN && res == DIR_RES_MISSED);
    snprintf(c.number, sizeof(c.number), "%s", num);
    return c;
}

static void test_history_ring(void)
{
    dir_call_t store[5];
    dir_hist_t h = { .e = store, .cap = 5 };
    TEST_ASSERT_NULL(dir_hist_get(&h, 0));
    char num[8];
    for (int i = 1; i <= 7; i++) {
        snprintf(num, sizeof(num), "%d", i);
        dir_call_t c = call(DIR_CALL_IN, i % 2 ? DIR_RES_MISSED : DIR_RES_ANSWERED, num, 1000 + i);
        dir_hist_add(&h, &c);
    }
    TEST_ASSERT_EQUAL(5, h.count);
    TEST_ASSERT_EQUAL_STRING("7", dir_hist_get(&h, 0)->number);     /* newest first */
    TEST_ASSERT_EQUAL_STRING("3", dir_hist_get(&h, 4)->number);     /* 1, 2 dropped */
    TEST_ASSERT_NULL(dir_hist_get(&h, 5));
    TEST_ASSERT_EQUAL(3, dir_hist_unseen_missed(&h));               /* 3, 5, 7 */
    TEST_ASSERT_TRUE(dir_hist_mark_seen(&h));
    TEST_ASSERT_EQUAL(0, dir_hist_unseen_missed(&h));
    TEST_ASSERT_FALSE(dir_hist_mark_seen(&h));
    dir_call_t out = call(DIR_CALL_OUT, DIR_RES_MISSED, "9", 0);    /* outgoing "missed" is never unseen */
    dir_hist_add(&h, &out);
    TEST_ASSERT_EQUAL(0, dir_hist_unseen_missed(&h));
}

static void test_history_serialize(void)
{
    dir_call_t a[DIR_HISTORY_MAX], b[DIR_HISTORY_MAX];
    dir_hist_t h = { .e = a, .cap = DIR_HISTORY_MAX };
    dir_hist_t g = { .e = b, .cap = DIR_HISTORY_MAX };
    for (int i = 0; i < 130; i++) {     /* wraps */
        char num[8];
        snprintf(num, sizeof(num), "%d", i);
        dir_call_t c = call(i & 1, i % 4, num, i ? 1700000000 + i : 0);
        c.duration_s = (uint32_t)i * 7;
        snprintf(c.name, sizeof(c.name), "N%d", i);
        dir_hist_add(&h, &c);
    }
    uint8_t *buf = malloc(dir_hist_size(&h));
    TEST_ASSERT_EQUAL(DIR_HIST_HDR_SIZE + DIR_HISTORY_MAX * DIR_HIST_REC_SIZE, dir_hist_size(&h));
    TEST_ASSERT_EQUAL(0, dir_hist_serialize(&h, buf, dir_hist_size(&h) - 1));
    size_t len = dir_hist_serialize(&h, buf, dir_hist_size(&h));
    TEST_ASSERT_EQUAL(dir_hist_size(&h), len);
    TEST_ASSERT_EQUAL(0, dir_hist_deserialize(&g, buf, len));
    TEST_ASSERT_EQUAL(h.count, g.count);
    for (size_t i = 0; i < h.count; i++) {
        const dir_call_t *x = dir_hist_get(&h, i), *y = dir_hist_get(&g, i);
        TEST_ASSERT_EQUAL(x->direction, y->direction);
        TEST_ASSERT_EQUAL(x->result, y->result);
        TEST_ASSERT_EQUAL(x->seen, y->seen);
        TEST_ASSERT_EQUAL_INT64(x->start, y->start);
        TEST_ASSERT_EQUAL_UINT32(x->duration_s, y->duration_s);
        TEST_ASSERT_EQUAL_STRING(x->number, y->number);
        TEST_ASSERT_EQUAL_STRING(x->name, y->name);
    }
    /* corrupt: flipped byte, truncated, wrong magic, garbage */
    uint8_t *bad = malloc(len);
    memcpy(bad, buf, len);
    bad[DIR_HIST_HDR_SIZE + 100] ^= 0x40;
    TEST_ASSERT_EQUAL(-1, dir_hist_deserialize(&g, bad, len));
    TEST_ASSERT_EQUAL(0, g.count);
    TEST_ASSERT_EQUAL(-1, dir_hist_deserialize(&g, buf, len - 1));
    TEST_ASSERT_EQUAL(-1, dir_hist_deserialize(&g, buf, 5));
    TEST_ASSERT_EQUAL(-1, dir_hist_deserialize(&g, buf, 0));
    memcpy(bad, buf, len);
    bad[0] = 'X';
    TEST_ASSERT_EQUAL(-1, dir_hist_deserialize(&g, bad, len));
    srand(3);
    for (int r = 0; r < 200; r++) {
        for (size_t i = 0; i < len; i++) {
            bad[i] = (uint8_t)rand();
        }
        TEST_ASSERT_EQUAL(-1, dir_hist_deserialize(&g, bad, len));
    }
    /* smaller ring than the file: newest entries kept */
    dir_call_t c3[3];
    dir_hist_t s = { .e = c3, .cap = 3 };
    TEST_ASSERT_EQUAL(0, dir_hist_deserialize(&s, buf, len));
    TEST_ASSERT_EQUAL(3, s.count);
    TEST_ASSERT_EQUAL_STRING("129", dir_hist_get(&s, 0)->number);
    TEST_ASSERT_EQUAL_STRING("127", dir_hist_get(&s, 2)->number);
    free(buf);
    free(bad);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_normalize);
    RUN_TEST(test_clean_name);
    RUN_TEST(test_contacts_sorted_update_delete);
    RUN_TEST(test_contacts_full);
    RUN_TEST(test_search);
    RUN_TEST(test_csv_lines);
    RUN_TEST(test_csv_format_roundtrip);
    RUN_TEST(test_csv_garbage);
    RUN_TEST(test_speed);
    RUN_TEST(test_history_ring);
    RUN_TEST(test_history_serialize);
    return UNITY_END();
}
