/* Host tests for components/keypad/keypad_scan.c (debounce and event logic).
 * Samples are fed every 10 ms, the scan period used by the firmware. */

#include <stddef.h>
#include <stdint.h>
#include "keypad_scan.h"
#include "unity.h"

#define KEY_A      0                   /* '1': row 0, col 0 */
#define KEY_B      4                   /* '5': row 1, col 1 */
#define BIT(k)     ((uint16_t)(1u << (k)))

typedef struct {
    uint32_t t;
    keypad_scan_type_t type;
    uint8_t key;
} rec_t;

typedef struct {
    keypad_scan_type_t type;
    uint32_t t;
} exp_t;

#define MAX_REC 256
static keypad_scan_state_t st;
static rec_t rec[MAX_REC];
static size_t nrec;

void setUp(void)
{
    keypad_scan_reset(&st);
    nrec = 0;
}

void tearDown(void) {}

/* One raw sample at time t, recording any events it produces. */
static void feed(uint32_t t, uint16_t raw)
{
    keypad_scan_event_t ev[KEYPAD_MAX_EVENTS];
    size_t n = keypad_scan_step(&st, raw, t, ev, KEYPAD_MAX_EVENTS);
    TEST_ASSERT_TRUE(n <= KEYPAD_MAX_EVENTS);
    for (size_t i = 0; i < n; i++) {
        TEST_ASSERT_TRUE(nrec < MAX_REC);
        rec[nrec].t = t;
        rec[nrec].type = ev[i].type;
        rec[nrec].key = ev[i].key;
        nrec++;
    }
}

/* Steady raw level for off in [from, to] (inclusive, step 10), relative to base.
 * Offsets are added in uint32 so base may sit near UINT32_MAX. */
static void hold(uint32_t base, uint32_t from, uint32_t to, uint16_t raw)
{
    for (uint32_t off = from; off <= to; off += 10) {
        feed(base + off, raw);
    }
}

/* Assert the events for one key match exactly, in order. Returns the count. */
static size_t expect_key(uint8_t key, const exp_t *exp, size_t n)
{
    size_t got = 0;
    for (size_t i = 0; i < nrec; i++) {
        if (rec[i].key != key) {
            continue;
        }
        TEST_ASSERT_TRUE_MESSAGE(got < n, "unexpected extra event for key");
        TEST_ASSERT_EQUAL_INT(exp[got].type, rec[i].type);
        TEST_ASSERT_EQUAL_UINT32(exp[got].t, rec[i].t);
        got++;
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE((int)n, (int)got, "event count for key");
    return got;
}

static void test_bounce_press_yields_single_press(void)
{
    /* Contact bounce for 30 ms, then settles pressed from t=40. */
    feed(0, BIT(KEY_A));
    feed(10, 0);
    feed(20, BIT(KEY_A));
    feed(30, 0);
    feed(40, BIT(KEY_A));
    hold(0, 50, 200, BIT(KEY_A));

    const exp_t e[] = { { KEYPAD_SCAN_PRESS, 70 } };
    TEST_ASSERT_EQUAL_INT(1, (int)expect_key(KEY_A, e, 1));
    TEST_ASSERT_EQUAL_INT(1, (int)nrec);
}

static void test_bounce_release_yields_single_release(void)
{
    hold(0, 0, 290, BIT(KEY_A));
    /* Release bounces between 300 and 320, settles released from 320. */
    feed(300, 0);
    feed(310, BIT(KEY_A));
    feed(320, 0);
    hold(0, 330, 500, 0);

    const exp_t e[] = {
        { KEYPAD_SCAN_PRESS, 30 },
        { KEYPAD_SCAN_RELEASE, 350 },
    };
    expect_key(KEY_A, e, 2);
    TEST_ASSERT_EQUAL_INT(2, (int)nrec);
}

static void test_short_press_no_long(void)
{
    hold(0, 0, 90, BIT(KEY_A));
    hold(0, 100, 300, 0);

    const exp_t e[] = {
        { KEYPAD_SCAN_PRESS, 30 },
        { KEYPAD_SCAN_RELEASE, 130 },
    };
    expect_key(KEY_A, e, 2);
    TEST_ASSERT_EQUAL_INT(2, (int)nrec);
}

/* Hold for 2 s from base: PRESS, LONG 800 ms after PRESS, REPEAT every 150 ms
 * from LONG while still down, RELEASE after the debounced release. */
static void check_hold(uint32_t base)
{
    hold(base, 0, 2000, BIT(KEY_A));
    hold(base, 2010, 2100, 0);

    exp_t e[11];
    e[0] = (exp_t){ KEYPAD_SCAN_PRESS, base + 30 };
    e[1] = (exp_t){ KEYPAD_SCAN_LONG, base + 830 };
    for (uint32_t k = 0; k < 8; k++) {
        e[2 + k] = (exp_t){ KEYPAD_SCAN_REPEAT, base + 980 + 150 * k };
    }
    e[10] = (exp_t){ KEYPAD_SCAN_RELEASE, base + 2040 };

    expect_key(KEY_A, e, 11);
    TEST_ASSERT_EQUAL_INT(11, (int)nrec);
}

static void test_hold_2s_long_and_repeats(void)
{
    check_hold(0);
}

static void test_hold_2s_across_uint32_wrap(void)
{
    /* base = UINT32_MAX - 500: PRESS and LONG both sit on the far side of the wrap. */
    check_hold(0xFFFFFE0Bu);
}

static void test_overlapping_keys_independent(void)
{
    /* A: down 0..490 (released at 530). B: down 200..1490 (held past LONG, released at 1530). */
    for (uint32_t off = 0; off <= 1600; off += 10) {
        uint16_t raw = 0;
        if (off < 500) {
            raw |= BIT(KEY_A);
        }
        if (off >= 200 && off < 1500) {
            raw |= BIT(KEY_B);
        }
        feed(off, raw);
    }

    const exp_t ea[] = {
        { KEYPAD_SCAN_PRESS, 30 },
        { KEYPAD_SCAN_RELEASE, 530 },
    };
    const exp_t eb[] = {
        { KEYPAD_SCAN_PRESS, 230 },
        { KEYPAD_SCAN_LONG, 1030 },
        { KEYPAD_SCAN_REPEAT, 1180 },
        { KEYPAD_SCAN_REPEAT, 1330 },
        { KEYPAD_SCAN_REPEAT, 1480 },
        { KEYPAD_SCAN_RELEASE, 1530 },
    };
    TEST_ASSERT_EQUAL_INT((int)nrec,
                          (int)(expect_key(KEY_A, ea, 2) + expect_key(KEY_B, eb, 6)));
}

static void test_all_twelve_keys_and_truncation(void)
{
    /* Bits above the 12 keys are ignored. */
    hold(0, 0, 100, 0xF000);
    TEST_ASSERT_EQUAL_INT(0, (int)nrec);

    /* All 12 keys down at once: 12 PRESS events in one step. */
    hold(0, 110, 150, 0x0FFF);
    TEST_ASSERT_EQUAL_INT(12, (int)nrec);
    for (uint8_t k = 0; k < KEYPAD_NUM_KEYS; k++) {
        TEST_ASSERT_EQUAL_INT(KEYPAD_SCAN_PRESS, rec[k].type);
        TEST_ASSERT_EQUAL_UINT32(140, rec[k].t);
        TEST_ASSERT_EQUAL_UINT8(k, rec[k].key);
    }

    /* Release all, but ask for room for 4 events: the return value still reports 12. */
    feed(200, 0);
    keypad_scan_event_t ev[4];
    size_t n = keypad_scan_step(&st, 0, 230, ev, 4);
    TEST_ASSERT_EQUAL_size_t(12, n);
    for (size_t i = 0; i < 4; i++) {
        TEST_ASSERT_EQUAL_INT(KEYPAD_SCAN_RELEASE, ev[i].type);
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_bounce_press_yields_single_press);
    RUN_TEST(test_bounce_release_yields_single_release);
    RUN_TEST(test_short_press_no_long);
    RUN_TEST(test_hold_2s_long_and_repeats);
    RUN_TEST(test_hold_2s_across_uint32_wrap);
    RUN_TEST(test_overlapping_keys_independent);
    RUN_TEST(test_all_twelve_keys_and_truncation);
    return UNITY_END();
}
