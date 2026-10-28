/* Host tests: touch-calibration math and sample capture (no LVGL). */

#include "ui_touch_cal.h"
#include "unity.h"

#define W 240
#define H 320
#define INSET 20

void setUp(void) {}
void tearDown(void) {}

/* A fake panel: raw = a - k * pixel on each axis (negative k = rotated 180 deg). */
typedef struct {
    int ax, kx, ay, ky;
} fake_t;

static ui_tcal_raw_t fake_raw(const fake_t *f, int32_t sx, int32_t sy)
{
    return (ui_tcal_raw_t){f->ax - f->kx * sx, f->ay - f->ky * sy};
}

static void fake_points(const fake_t *f, ui_tcal_raw_t raw[UI_TCAL_POINTS])
{
    for (int i = 0; i < UI_TCAL_POINTS; i++) {
        int32_t x, y;
        ui_tcal_target(i, W, H, INSET, &x, &y);
        raw[i] = fake_raw(f, x, y);
    }
}

static void test_targets(void)
{
    int32_t x, y;
    ui_tcal_target(0, W, H, INSET, &x, &y);
    TEST_ASSERT_EQUAL_INT32(20, x);
    TEST_ASSERT_EQUAL_INT32(20, y);
    ui_tcal_target(1, W, H, INSET, &x, &y);
    TEST_ASSERT_EQUAL_INT32(219, x);
    TEST_ASSERT_EQUAL_INT32(20, y);
    ui_tcal_target(2, W, H, INSET, &x, &y);
    TEST_ASSERT_EQUAL_INT32(219, x);
    TEST_ASSERT_EQUAL_INT32(299, y);
    ui_tcal_target(3, W, H, INSET, &x, &y);
    TEST_ASSERT_EQUAL_INT32(20, x);
    TEST_ASSERT_EQUAL_INT32(299, y);
}

/* raw = 3900 - 15 x, 3900 - 11 y: the simulator's 180 degree fake. */
static void test_rotated_180(void)
{
    const fake_t f = {3900, 15, 3900, 11};
    ui_tcal_raw_t raw[UI_TCAL_POINTS];
    fake_points(&f, raw);
    ui_touch_cal_t c;
    TEST_ASSERT_EQUAL(UI_TCAL_OK, ui_tcal_compute(raw, W, H, INSET, &c));
    TEST_ASSERT_INT16_WITHIN(1, 3900, c.x_min);
    TEST_ASSERT_INT16_WITHIN(1, 3900 - 15 * (W - 1), c.x_max);
    TEST_ASSERT_INT16_WITHIN(1, 3900, c.y_min);
    TEST_ASSERT_INT16_WITHIN(1, 3900 - 11 * (H - 1), c.y_max);
    TEST_ASSERT_TRUE(c.x_min > c.x_max);
    TEST_ASSERT_TRUE(c.y_min > c.y_max);
}

static void test_normal_orientation(void)
{
    const fake_t f = {-200, -14, 150, -12};   /* raw = -200 + 14 x, 150 + 12 y */
    ui_tcal_raw_t raw[UI_TCAL_POINTS];
    fake_points(&f, raw);
    ui_touch_cal_t c;
    TEST_ASSERT_EQUAL(UI_TCAL_OK, ui_tcal_compute(raw, W, H, INSET, &c));
    TEST_ASSERT_INT16_WITHIN(1, -200, c.x_min);
    TEST_ASSERT_INT16_WITHIN(1, -200 + 14 * (W - 1), c.x_max);
    TEST_ASSERT_INT16_WITHIN(1, 150, c.y_min);
    TEST_ASSERT_INT16_WITHIN(1, 150 + 12 * (H - 1), c.y_max);
}

/* Mirrored in one axis only. */
static void test_one_axis_inverted(void)
{
    const fake_t f = {100, -13, 3800, 11};
    ui_tcal_raw_t raw[UI_TCAL_POINTS];
    fake_points(&f, raw);
    ui_touch_cal_t c;
    TEST_ASSERT_EQUAL(UI_TCAL_OK, ui_tcal_compute(raw, W, H, INSET, &c));
    TEST_ASSERT_TRUE(c.x_min < c.x_max);
    TEST_ASSERT_TRUE(c.y_min > c.y_max);
}

/* The computed calibration maps the targets back to their pixels. */
static void test_roundtrip(void)
{
    const fake_t f = {3900, 15, 3900, 11};
    ui_tcal_raw_t raw[UI_TCAL_POINTS];
    fake_points(&f, raw);
    ui_touch_cal_t c;
    TEST_ASSERT_EQUAL(UI_TCAL_OK, ui_tcal_compute(raw, W, H, INSET, &c));
    for (int i = 0; i < UI_TCAL_POINTS; i++) {
        int32_t x, y;
        ui_tcal_target(i, W, H, INSET, &x, &y);
        TEST_ASSERT_INT32_WITHIN(1, x, ui_tcal_map(raw[i].x, c.x_min, c.x_max, W));
        TEST_ASSERT_INT32_WITHIN(1, y, ui_tcal_map(raw[i].y, c.y_min, c.y_max, H));
    }
    /* edges and clamping */
    TEST_ASSERT_EQUAL_INT32(0, ui_tcal_map(c.x_min, c.x_min, c.x_max, W));
    TEST_ASSERT_EQUAL_INT32(W - 1, ui_tcal_map(c.x_max, c.x_min, c.x_max, W));
    TEST_ASSERT_EQUAL_INT32(0, ui_tcal_map(5000, c.x_min, c.x_max, W));
    TEST_ASSERT_EQUAL_INT32(W - 1, ui_tcal_map(-5000, c.x_min, c.x_max, W));
    TEST_ASSERT_EQUAL_INT32(0, ui_tcal_map(1234, 7, 7, W));
}

static void test_noise_tolerated(void)
{
    const fake_t f = {3900, 15, 3900, 11};
    ui_tcal_raw_t raw[UI_TCAL_POINTS];
    fake_points(&f, raw);
    raw[0].x += 60;
    raw[1].y -= 80;
    raw[2].x += 50;
    raw[3].y += 70;
    ui_touch_cal_t c;
    TEST_ASSERT_EQUAL(UI_TCAL_OK, ui_tcal_compute(raw, W, H, INSET, &c));
    TEST_ASSERT_INT16_WITHIN(80, 3900, c.x_min);
    TEST_ASSERT_INT16_WITHIN(80, 3900 - 11 * (H - 1), c.y_max);
}

static void test_fail_span(void)
{
    ui_tcal_raw_t raw[UI_TCAL_POINTS] = {{2000, 2000}, {2400, 2000}, {2400, 2900}, {2000, 2900}};
    ui_touch_cal_t c = {1, 2, 3, 4};
    TEST_ASSERT_EQUAL(UI_TCAL_ERR_SPAN, ui_tcal_compute(raw, W, H, INSET, &c));
    TEST_ASSERT_EQUAL_INT16(1, c.x_min);   /* untouched on failure */
    TEST_ASSERT_EQUAL_INT16(4, c.y_max);
}

static void test_fail_all_same_point(void)
{
    ui_tcal_raw_t raw[UI_TCAL_POINTS] = {{1500, 1500}, {1500, 1500}, {1500, 1500}, {1500, 1500}};
    ui_touch_cal_t c;
    TEST_ASSERT_EQUAL(UI_TCAL_ERR_SPAN, ui_tcal_compute(raw, W, H, INSET, &c));
}

static void test_fail_axes_swapped(void)
{
    /* touch X runs along screen Y: left and right edge look the same */
    ui_tcal_raw_t raw[UI_TCAL_POINTS];
    for (int i = 0; i < UI_TCAL_POINTS; i++) {
        int32_t x, y;
        ui_tcal_target(i, W, H, INSET, &x, &y);
        raw[i].x = 300 + y * 11;
        raw[i].y = 300 + x * 14;
    }
    ui_touch_cal_t c;
    TEST_ASSERT_EQUAL(UI_TCAL_ERR_SPAN, ui_tcal_compute(raw, W, H, INSET, &c));
}

static void test_fail_skew_single_point(void)
{
    const fake_t f = {3900, 15, 3900, 11};
    ui_tcal_raw_t raw[UI_TCAL_POINTS];
    fake_points(&f, raw);
    raw[2].x += 450;   /* BR way off from TR */
    ui_touch_cal_t c;
    TEST_ASSERT_EQUAL(UI_TCAL_ERR_SKEW, ui_tcal_compute(raw, W, H, INSET, &c));

    fake_points(&f, raw);
    raw[3].y -= 450;   /* BL.y vs BR.y */
    TEST_ASSERT_EQUAL(UI_TCAL_ERR_SKEW, ui_tcal_compute(raw, W, H, INSET, &c));
}

static void test_fail_range(void)
{
    /* huge slope: extrapolating to the edges leaves the 12-bit range */
    ui_tcal_raw_t raw[UI_TCAL_POINTS] = {{2000, 100}, {2100, 100}, {2100, 4000}, {2000, 4000}};
    ui_touch_cal_t c;
    TEST_ASSERT_EQUAL(UI_TCAL_ERR_SPAN, ui_tcal_compute(raw, W, H, INSET, &c));
    ui_tcal_raw_t raw2[UI_TCAL_POINTS] = {{1000, 100}, {2100, 100}, {2100, 3900}, {1000, 3900}};
    /* x extrapolates to ~ 1000 - 1100*20/199 = 890 .. 2210: fine; y fine => OK */
    TEST_ASSERT_EQUAL(UI_TCAL_OK, ui_tcal_compute(raw2, W, H, INSET, &c));
    ui_tcal_raw_t raw3[UI_TCAL_POINTS] = {{4000, 100}, {5000, 100}, {5000, 3900}, {4000, 3900}};
    TEST_ASSERT_EQUAL(UI_TCAL_ERR_RANGE, ui_tcal_compute(raw3, W, H, INSET, &c));
}

static void test_bad_args(void)
{
    ui_tcal_raw_t raw[UI_TCAL_POINTS] = {{0}};
    ui_touch_cal_t c;
    TEST_ASSERT_EQUAL(UI_TCAL_ERR_ARG, ui_tcal_compute(NULL, W, H, INSET, &c));
    TEST_ASSERT_EQUAL(UI_TCAL_ERR_ARG, ui_tcal_compute(raw, W, H, INSET, NULL));
    TEST_ASSERT_EQUAL(UI_TCAL_ERR_ARG, ui_tcal_compute(raw, W, H, 150, &c));
}

static void test_capture_average(void)
{
    ui_tcal_cap_t c;
    ui_tcal_cap_reset(&c);
    ui_tcal_raw_t p = {0, 0};
    TEST_ASSERT_FALSE(ui_tcal_cap_feed(&c, 1000, 2000, true, &p));
    TEST_ASSERT_FALSE(ui_tcal_cap_feed(&c, 1002, 2004, true, &p));
    TEST_ASSERT_FALSE(ui_tcal_cap_feed(&c, 1004, 2002, true, &p));
    TEST_ASSERT_FALSE(ui_tcal_cap_feed(&c, 1002, 2002, true, &p));
    TEST_ASSERT_TRUE(ui_tcal_cap_feed(&c, 0, 0, false, &p));
    TEST_ASSERT_EQUAL_INT32(1002, p.x);
    TEST_ASSERT_EQUAL_INT32(2002, p.y);
    /* one tap = one point: a second release does nothing */
    TEST_ASSERT_FALSE(ui_tcal_cap_feed(&c, 0, 0, false, &p));
}

static void test_capture_short_press_ignored(void)
{
    ui_tcal_cap_t c;
    ui_tcal_cap_reset(&c);
    ui_tcal_raw_t p;
    ui_tcal_cap_feed(&c, 500, 500, true, &p);
    ui_tcal_cap_feed(&c, 500, 500, true, &p);
    TEST_ASSERT_FALSE(ui_tcal_cap_feed(&c, 0, 0, false, &p));
    /* and the short press leaves nothing behind */
    for (int i = 0; i < UI_TCAL_MIN_SAMPLES; i++) {
        ui_tcal_cap_feed(&c, 3000, 3100, true, &p);
    }
    TEST_ASSERT_TRUE(ui_tcal_cap_feed(&c, 0, 0, false, &p));
    TEST_ASSERT_EQUAL_INT32(3000, p.x);
    TEST_ASSERT_EQUAL_INT32(3100, p.y);
}

/* A whole session through the capture, 180 degree fake panel. */
static void test_full_session(void)
{
    const fake_t f = {3900, 15, 3900, 11};
    ui_tcal_cap_t cap;
    ui_tcal_cap_reset(&cap);
    ui_tcal_raw_t pts[UI_TCAL_POINTS];
    for (int i = 0; i < UI_TCAL_POINTS; i++) {
        int32_t x, y;
        ui_tcal_target(i, W, H, INSET, &x, &y);
        ui_tcal_raw_t got;
        for (int s = 0; s < 6; s++) {
            ui_tcal_raw_t r = fake_raw(&f, x + (s % 3) - 1, y + (s % 2));   /* a jittery finger */
            TEST_ASSERT_FALSE(ui_tcal_cap_feed(&cap, r.x, r.y, true, &got));
        }
        TEST_ASSERT_TRUE(ui_tcal_cap_feed(&cap, 0, 0, false, &got));
        pts[i] = got;
    }
    ui_touch_cal_t c;
    TEST_ASSERT_EQUAL(UI_TCAL_OK, ui_tcal_compute(pts, W, H, INSET, &c));
    TEST_ASSERT_INT16_WITHIN(25, 3900, c.x_min);
    TEST_ASSERT_INT16_WITHIN(25, 3900 - 15 * (W - 1), c.x_max);
    TEST_ASSERT_INT16_WITHIN(25, 3900, c.y_min);
    TEST_ASSERT_INT16_WITHIN(25, 3900 - 11 * (H - 1), c.y_max);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_targets);
    RUN_TEST(test_rotated_180);
    RUN_TEST(test_normal_orientation);
    RUN_TEST(test_one_axis_inverted);
    RUN_TEST(test_roundtrip);
    RUN_TEST(test_noise_tolerated);
    RUN_TEST(test_fail_span);
    RUN_TEST(test_fail_all_same_point);
    RUN_TEST(test_fail_axes_swapped);
    RUN_TEST(test_fail_skew_single_point);
    RUN_TEST(test_fail_range);
    RUN_TEST(test_bad_args);
    RUN_TEST(test_capture_average);
    RUN_TEST(test_capture_short_press_ignored);
    RUN_TEST(test_full_session);
    return UNITY_END();
}
