#include "status_led_pattern.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static sl_state_t idle_registered(void)
{
    return (sl_state_t){ .booting = false, .net = SL_NET_UP, .reg = SL_REG_REGISTERED, .call = SL_CALL_IDLE };
}

static bool is_off(sl_rgb_t c)
{
    return c.r == 0 && c.g == 0 && c.b == 0;
}

static void test_booting_white_solid(void)
{
    sl_state_t st = { .booting = true };
    sl_rgb_t c = sl_pattern(&st, 1234, 255);
    TEST_ASSERT_EQUAL(255, c.r);
    TEST_ASSERT_EQUAL(255, c.g);
    TEST_ASSERT_EQUAL(255, c.b);
}

static void test_no_network_red_blink(void)
{
    sl_state_t st = { .net = SL_NET_DOWN };
    sl_rgb_t on = sl_pattern(&st, 100, 255);
    TEST_ASSERT_EQUAL(255, on.r);
    TEST_ASSERT_EQUAL(0, on.g);
    TEST_ASSERT_TRUE(is_off(sl_pattern(&st, 500, 255)));
    TEST_ASSERT_FALSE(is_off(sl_pattern(&st, 2100, 255)));
}

static void test_registered_heartbeat(void)
{
    sl_state_t st = idle_registered();
    TEST_ASSERT_EQUAL(255, sl_pattern(&st, 50, 255).g);
    TEST_ASSERT_TRUE(is_off(sl_pattern(&st, 200, 255)));
    TEST_ASSERT_TRUE(is_off(sl_pattern(&st, 4999, 255)));
    TEST_ASSERT_EQUAL(255, sl_pattern(&st, 5050, 255).g);
}

static void test_reg_failed_red_solid(void)
{
    sl_state_t st = idle_registered();
    st.reg = SL_REG_FAILED;
    for (uint32_t t = 0; t < 3000; t += 250) {
        TEST_ASSERT_EQUAL(255, sl_pattern(&st, t, 255).r);
    }
}

static void test_call_overrides_network(void)
{
    sl_state_t st = { .net = SL_NET_DOWN, .booting = true, .call = SL_CALL_ACTIVE };
    sl_rgb_t c = sl_pattern(&st, 777, 255);
    TEST_ASSERT_EQUAL(0, c.r);
    TEST_ASSERT_EQUAL(255, c.g);
}

static void test_incoming_fast_blue_blink(void)
{
    sl_state_t st = idle_registered();
    st.call = SL_CALL_INCOMING;
    TEST_ASSERT_EQUAL(255, sl_pattern(&st, 0, 255).b);
    TEST_ASSERT_TRUE(is_off(sl_pattern(&st, 200, 255)));
    TEST_ASSERT_EQUAL(255, sl_pattern(&st, 300, 255).b);
}

static void test_brightness_scaling(void)
{
    sl_state_t st = { .booting = true };
    sl_rgb_t c = sl_pattern(&st, 0, 40);
    TEST_ASSERT_EQUAL(40, c.r);
    TEST_ASSERT_TRUE(is_off(sl_pattern(&st, 0, 0)));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_booting_white_solid);
    RUN_TEST(test_no_network_red_blink);
    RUN_TEST(test_registered_heartbeat);
    RUN_TEST(test_reg_failed_red_solid);
    RUN_TEST(test_call_overrides_network);
    RUN_TEST(test_incoming_fast_blue_blink);
    RUN_TEST(test_brightness_scaling);
    return UNITY_END();
}
