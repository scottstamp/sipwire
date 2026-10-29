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

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_today);
    RUN_TEST(test_weekday_and_date);
    RUN_TEST(test_month_and_year_boundary);
    return UNITY_END();
}
