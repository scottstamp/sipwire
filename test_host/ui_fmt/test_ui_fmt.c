#include <string.h>
#include <time.h>

#include "ui_fmt.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

/* tm for 2026-10-29 (Thursday) hh:mm */
static struct tm mk(int year, int mon, int day, int wday, int h, int m)
{
    struct tm t;
    memset(&t, 0, sizeof(t));
    t.tm_year = year - 1900;
    t.tm_mon = mon - 1;
    t.tm_mday = day;
    t.tm_wday = wday;
    t.tm_hour = h;
    t.tm_min = m;
    return t;
}

static void test_today(void)
{
    struct tm now = mk(2026, 10, 29, 4, 15, 0), t = mk(2026, 10, 29, 4, 14, 3), p = mk(2026, 10, 29, 4, 0, 5);
    char b[16];
    ui_fmt_when(b, sizeof(b), &t, &now, true);
    TEST_ASSERT_EQUAL_STRING("14:03", b);
    ui_fmt_when(b, sizeof(b), &t, &now, false);
    TEST_ASSERT_EQUAL_STRING("2:03 PM", b);
    ui_fmt_when(b, sizeof(b), &p, &now, false);
    TEST_ASSERT_EQUAL_STRING("12:05 AM", b);
}

static void test_weekday_and_date(void)
{
    struct tm now = mk(2026, 10, 29, 4, 15, 0);
    char b[16];
    struct tm y = mk(2026, 10, 28, 3, 23, 59);
    ui_fmt_when(b, sizeof(b), &y, &now, true);
    TEST_ASSERT_EQUAL_STRING("Wed", b);
    struct tm w = mk(2026, 10, 23, 5, 9, 0);   /* 6 days ago */
    ui_fmt_when(b, sizeof(b), &w, &now, true);
    TEST_ASSERT_EQUAL_STRING("Fri", b);
    struct tm w7 = mk(2026, 10, 22, 4, 9, 0);  /* a week ago: same weekday as today, so a date */
    ui_fmt_when(b, sizeof(b), &w7, &now, true);
    TEST_ASSERT_EQUAL_STRING("Oct 22", b);
    struct tm old = mk(2026, 3, 5, 4, 9, 0);
    ui_fmt_when(b, sizeof(b), &old, &now, true);
    TEST_ASSERT_EQUAL_STRING("Mar 5", b);
}

static void test_month_and_year_boundary(void)
{
    struct tm now = mk(2027, 1, 2, 6, 8, 0);
    char b[16];
    struct tm t = mk(2026, 12, 31, 4, 23, 0);   /* 2 days ago across the year */
    ui_fmt_when(b, sizeof(b), &t, &now, true);
    TEST_ASSERT_EQUAL_STRING("Thu", b);
    struct tm f = mk(2027, 1, 5, 2, 8, 0);      /* in the future: date */
    ui_fmt_when(b, sizeof(b), &f, &now, true);
    TEST_ASSERT_EQUAL_STRING("Jan 5", b);
}

static ui_call_t hc(const char *num, int64_t start, uint8_t res, bool seen)
{
    ui_call_t c;
    memset(&c, 0, sizeof(c));
    c.direction = UI_CALLDIR_IN;
    c.result = res;
    c.seen = seen;
    c.start = start;
    strcpy(c.number, num);
    return c;
}

static void test_hist_shift_and_fresh(void)
{
    /* opened with: B (missed, unseen), A (answered); then marked seen */
    ui_call_t old[2] = {hc("1002", 200, UI_CALLRES_MISSED, false), hc("1001", 100, UI_CALLRES_ANSWERED, true)};
    bool fresh[8] = {false};
    ui_hist_carry_fresh(fresh, 0, -1, old, 2);
    TEST_ASSERT_TRUE(fresh[0]);
    TEST_ASSERT_FALSE(fresh[1]);
    old[0].seen = true;                             /* refetch after mark seen: nothing added */
    TEST_ASSERT_EQUAL_INT32(0, ui_hist_shift(&old[0], old, 2));
    ui_hist_carry_fresh(fresh, 2, 0, old, 2);
    TEST_ASSERT_TRUE(fresh[0]);                     /* still highlighted */
    TEST_ASSERT_FALSE(fresh[1]);

    /* two new calls in front: a missed (unseen) one and an outgoing one */
    ui_call_t now[4] = {hc("1003", 400, UI_CALLRES_MISSED, false), hc("1004", 300, UI_CALLRES_ANSWERED, true),
                        old[0], old[1]};
    now[1].direction = UI_CALLDIR_OUT;
    int32_t k = ui_hist_shift(&old[0], now, 4);
    TEST_ASSERT_EQUAL_INT32(2, k);
    ui_hist_carry_fresh(fresh, 2, k, now, 4);
    TEST_ASSERT_TRUE(fresh[0]);
    TEST_ASSERT_FALSE(fresh[1]);
    TEST_ASSERT_TRUE(fresh[2]);                     /* the old B kept its flag */
    TEST_ASSERT_FALSE(fresh[3]);

    /* same number, other time: not the same entry; cleared history: -1 */
    ui_call_t other = hc("1002", 201, UI_CALLRES_MISSED, true);
    TEST_ASSERT_EQUAL_INT32(-1, ui_hist_shift(&other, now, 4));
    TEST_ASSERT_EQUAL_INT32(-1, ui_hist_shift(&old[0], now, 0));
    ui_hist_carry_fresh(fresh, 4, -1, now, 1);
    TEST_ASSERT_TRUE(fresh[0]);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_today);
    RUN_TEST(test_weekday_and_date);
    RUN_TEST(test_month_and_year_boundary);
    RUN_TEST(test_hist_shift_and_fresh);
    return UNITY_END();
}
