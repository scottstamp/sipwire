/* Host tests: unlock swipe gesture and in-call touch lock eligibility (no LVGL). */

#include <string.h>

#include "ui_lock_logic.h"
#include "unity.h"

#define TRAVEL 166

void setUp(void) {}
void tearDown(void) {}

/* Drag from x0 to x1 at row y in steps of `step` px; returns the last state. */
static ui_swipe_state_t drag(ui_swipe_t *s, int32_t x0, int32_t x1, int32_t y, int32_t step)
{
    ui_swipe_press(s, TRAVEL, x0, y);
    ui_swipe_state_t st = s->state;
    for (int32_t x = x0 + step; st == UI_SWIPE_DRAGGING; x += step) {
        st = ui_swipe_move(s, x > x1 ? x1 : x, y);
        if (x >= x1) {
            break;
        }
    }
    return st;
}

static void test_full_swipe_unlocks(void)
{
    ui_swipe_t s;
    TEST_ASSERT_EQUAL(UI_SWIPE_DONE, drag(&s, 36, 36 + TRAVEL, 293, 20));
    TEST_ASSERT_EQUAL(TRAVEL, s.pos);
    /* further samples change nothing, DONE is reported once by the caller's edge check */
    TEST_ASSERT_EQUAL(UI_SWIPE_DONE, ui_swipe_move(&s, 300, 293));
    ui_swipe_release(&s);
    TEST_ASSERT_EQUAL(UI_SWIPE_IDLE, s.state);
    TEST_ASSERT_EQUAL(0, s.pos);
}

static void test_end_slack(void)
{
    ui_swipe_t s;
    TEST_ASSERT_EQUAL(UI_SWIPE_DONE, drag(&s, 36, 36 + TRAVEL - UI_SWIPE_END_SLACK, 293, 20));
    TEST_ASSERT_EQUAL(UI_SWIPE_DRAGGING, drag(&s, 36, 36 + TRAVEL - UI_SWIPE_END_SLACK - 1, 293, 20));
}

static void test_half_swipe_springs_back(void)
{
    ui_swipe_t s;
    TEST_ASSERT_EQUAL(UI_SWIPE_DRAGGING, drag(&s, 36, 136, 293, 20));
    TEST_ASSERT_EQUAL(100, s.pos);
    ui_swipe_release(&s);
    TEST_ASSERT_EQUAL(0, s.pos);
}

static void test_left_and_clamp(void)
{
    ui_swipe_t s;
    ui_swipe_press(&s, TRAVEL, 50, 293);
    TEST_ASSERT_EQUAL(UI_SWIPE_DRAGGING, ui_swipe_move(&s, 20, 293));
    TEST_ASSERT_EQUAL(0, s.pos);
}

static void test_jump_aborts(void)
{
    ui_swipe_t s;
    ui_swipe_press(&s, TRAVEL, 36, 293);
    TEST_ASSERT_EQUAL(UI_SWIPE_DRAGGING, ui_swipe_move(&s, 56, 293));
    /* a second contact: the averaged point leaps */
    TEST_ASSERT_EQUAL(UI_SWIPE_ABORTED, ui_swipe_move(&s, 56 + UI_SWIPE_MAX_STEP + 1, 293));
    TEST_ASSERT_EQUAL(0, s.pos);
    /* stays aborted until the next press, even when the finger reaches the end */
    TEST_ASSERT_EQUAL(UI_SWIPE_ABORTED, ui_swipe_move(&s, 36 + TRAVEL, 293));
    TEST_ASSERT_EQUAL(0, s.pos);
    ui_swipe_release(&s);
    TEST_ASSERT_EQUAL(UI_SWIPE_DONE, drag(&s, 36, 36 + TRAVEL, 293, 20));
}

static void test_vertical_jump_aborts(void)
{
    ui_swipe_t s;
    ui_swipe_press(&s, TRAVEL, 36, 293);
    TEST_ASSERT_EQUAL(UI_SWIPE_ABORTED, ui_swipe_move(&s, 40, 293 - UI_SWIPE_MAX_STEP - 1));
}

static void test_drift_off_track_aborts(void)
{
    ui_swipe_t s;
    ui_swipe_press(&s, TRAVEL, 36, 293);
    int32_t y = 293;
    ui_swipe_state_t st = UI_SWIPE_DRAGGING;
    for (int32_t x = 46; st == UI_SWIPE_DRAGGING && x < 36 + TRAVEL; x += 10) {
        y -= 5;     /* diagonal: slow steps, but off the row */
        st = ui_swipe_move(&s, x, y);
    }
    TEST_ASSERT_EQUAL(UI_SWIPE_ABORTED, st);
}

static void test_move_without_press_ignored(void)
{
    ui_swipe_t s;
    memset(&s, 0, sizeof(s));
    TEST_ASSERT_EQUAL(UI_SWIPE_IDLE, ui_swipe_move(&s, 300, 293));
    TEST_ASSERT_EQUAL(0, s.pos);
}

static void test_call_lock_eligible(void)
{
    ui_model_t m;
    memset(&m, 0, sizeof(m));
    m.call_touch_lock_s = 5;
    m.call = UI_CALL_ACTIVE;
    TEST_ASSERT_TRUE(ui_call_lock_eligible(&m));

    m.speaker = true;
    TEST_ASSERT_FALSE(ui_call_lock_eligible(&m));
    m.speaker = false;

    m.second_call = UI_CALL_RINGING_IN;     /* call waiting prompt must be answerable */
    TEST_ASSERT_FALSE(ui_call_lock_eligible(&m));
    m.second_call = UI_CALL_HELD;           /* a held second call does not matter */
    TEST_ASSERT_TRUE(ui_call_lock_eligible(&m));

    m.call_touch_lock_s = 0;
    TEST_ASSERT_FALSE(ui_call_lock_eligible(&m));
    m.call_touch_lock_s = 5;

    const ui_call_state_t no[] = {UI_CALL_IDLE, UI_CALL_DIALING, UI_CALL_RINGING_OUT, UI_CALL_RINGING_IN,
                                  UI_CALL_HELD, UI_CALL_ENDED};
    for (size_t i = 0; i < sizeof(no) / sizeof(no[0]); i++) {
        m.call = no[i];
        TEST_ASSERT_FALSE(ui_call_lock_eligible(&m));
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_full_swipe_unlocks);
    RUN_TEST(test_end_slack);
    RUN_TEST(test_half_swipe_springs_back);
    RUN_TEST(test_left_and_clamp);
    RUN_TEST(test_jump_aborts);
    RUN_TEST(test_vertical_jump_aborts);
    RUN_TEST(test_drift_off_track_aborts);
    RUN_TEST(test_move_without_press_ignored);
    RUN_TEST(test_call_lock_eligible);
    return UNITY_END();
}
