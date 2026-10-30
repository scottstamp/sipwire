#include <string.h>

#include "unity.h"
#include "net_policy.h"

void setUp(void) {}
void tearDown(void) {}

static const net_roam_cfg_t ROAM = {
    .idle_dbm = -70, .call_dbm = -80, .delta_db = 8, .interval_ms = 30000, .max_backoff = 3,
};

static const net_warn_cfg_t WARN = {
    .warn_dbm = -75, .hyst_db = 5, .repeat_ms = 10000, .enter_samples = 2,
};

static const uint8_t AP_A[6] = { 0x02, 0, 0, 0, 0, 0xa };
static const uint8_t AP_B[6] = { 0x02, 0, 0, 0, 0, 0xb };
static const uint8_t AP_C[6] = { 0x02, 0, 0, 0, 0, 0xc };

/* ---- roaming ---- */

static void test_roam_not_before_interval_after_connect(void)
{
    net_roam_t r;
    net_roam_reset(&r, &ROAM, 1000);
    TEST_ASSERT_FALSE(net_roam_scan_due(&r, &ROAM, false, -85, 1000));
    TEST_ASSERT_FALSE(net_roam_scan_due(&r, &ROAM, false, -85, 30999));
    TEST_ASSERT_TRUE(net_roam_scan_due(&r, &ROAM, false, -85, 31000));
}

static void test_roam_thresholds_idle_vs_call(void)
{
    net_roam_t r;
    net_roam_reset(&r, &ROAM, 0);
    uint64_t t = 60000;
    TEST_ASSERT_FALSE(net_roam_scan_due(&r, &ROAM, false, -70, t));   /* at the trigger: fine */
    TEST_ASSERT_TRUE(net_roam_scan_due(&r, &ROAM, false, -71, t));
    /* a call tolerates more before risking an audio gap */
    TEST_ASSERT_FALSE(net_roam_scan_due(&r, &ROAM, true, -75, t));
    TEST_ASSERT_FALSE(net_roam_scan_due(&r, &ROAM, true, -80, t));
    TEST_ASSERT_TRUE(net_roam_scan_due(&r, &ROAM, true, -81, t));
    TEST_ASSERT_FALSE(net_roam_scan_due(&r, &ROAM, false, NET_RSSI_NONE, t));
}

static void test_roam_scan_picks_strongest_other_ap(void)
{
    net_roam_scan_t s;
    net_roam_scan_init(&s);
    net_roam_scan_add(&s, AP_A, AP_A, -78, 1);
    net_roam_scan_add(&s, AP_A, AP_B, -66, 6);
    net_roam_scan_add(&s, AP_A, AP_C, -60, 11);
    net_roam_scan_add(&s, AP_A, AP_B, -64, 6);
    TEST_ASSERT_EQUAL(4, s.seen);
    TEST_ASSERT_EQUAL(-78, s.cur_rssi);
    TEST_ASSERT_EQUAL(-60, s.rssi);
    TEST_ASSERT_EQUAL(11, s.channel);
    TEST_ASSERT_EQUAL_MEMORY(AP_C, s.bssid, 6);
}

static void test_roam_needs_delta_over_stronger_reading(void)
{
    net_roam_t r;
    net_roam_reset(&r, &ROAM, 0);
    net_roam_scan_t s;
    net_roam_scan_init(&s);
    net_roam_scan_add(&s, AP_A, AP_A, -74, 1);
    net_roam_scan_add(&s, AP_A, AP_B, -67, 6);
    /* driver says -80, scan says -74: the stronger counts, B is only 7 dB better */
    TEST_ASSERT_FALSE(net_roam_evaluate(&r, &ROAM, &s, -80, 1000));
    net_roam_scan_add(&s, AP_A, AP_B, -66, 6);
    TEST_ASSERT_TRUE(net_roam_evaluate(&r, &ROAM, &s, -80, 1000));
    TEST_ASSERT_EQUAL(0, r.backoff);
}

static void test_roam_single_ap_backs_off(void)
{
    net_roam_t r;
    net_roam_reset(&r, &ROAM, 0);
    net_roam_scan_t s;
    net_roam_scan_init(&s);
    net_roam_scan_add(&s, AP_A, AP_A, -82, 1);      /* only our own AP */
    uint64_t t = 30000;
    uint64_t expect[] = { 60000, 120000, 240000, 240000 };
    for (int i = 0; i < 4; i++) {
        TEST_ASSERT_TRUE(net_roam_scan_due(&r, &ROAM, false, -82, t));
        TEST_ASSERT_FALSE(net_roam_evaluate(&r, &ROAM, &s, -82, t));
        TEST_ASSERT_EQUAL_UINT64(t + expect[i], r.next_scan_ms);
        TEST_ASSERT_FALSE(net_roam_scan_due(&r, &ROAM, false, -82, t + expect[i] - 1));
        t += expect[i];
    }
    /* signal recovers: the next weak episode starts at the base interval again */
    TEST_ASSERT_FALSE(net_roam_scan_due(&r, &ROAM, false, -60, t));
    TEST_ASSERT_EQUAL(0, r.backoff);
    TEST_ASSERT_TRUE(net_roam_scan_due(&r, &ROAM, false, -75, t));
    TEST_ASSERT_FALSE(net_roam_evaluate(&r, &ROAM, &s, -75, t));
    TEST_ASSERT_EQUAL_UINT64(t + 60000, r.next_scan_ms);
}

static void test_roam_empty_scan(void)
{
    net_roam_t r;
    net_roam_reset(&r, &ROAM, 0);
    net_roam_scan_t s;
    net_roam_scan_init(&s);
    TEST_ASSERT_FALSE(net_roam_evaluate(&r, &ROAM, &s, -85, 0));
    TEST_ASSERT_FALSE(net_roam_evaluate(&r, &ROAM, &s, NET_RSSI_NONE, 0));
}

/* ---- weak-signal warning ---- */

static void test_warn_enter_needs_consecutive_samples(void)
{
    net_warn_t w;
    net_warn_reset(&w);
    TEST_ASSERT_EQUAL(NET_WARN_NONE, net_warn_update(&w, &WARN, true, -76, 0));
    TEST_ASSERT_EQUAL(NET_WARN_NONE, net_warn_update(&w, &WARN, true, -70, 2000));  /* blip */
    TEST_ASSERT_EQUAL(NET_WARN_NONE, net_warn_update(&w, &WARN, true, -76, 4000));
    TEST_ASSERT_EQUAL(NET_WARN_ENTER, net_warn_update(&w, &WARN, true, -77, 6000));
    TEST_ASSERT_TRUE(w.weak);
}

static void test_warn_repeats_and_hysteresis(void)
{
    net_warn_t w;
    net_warn_reset(&w);
    net_warn_update(&w, &WARN, true, -80, 0);
    TEST_ASSERT_EQUAL(NET_WARN_ENTER, net_warn_update(&w, &WARN, true, -80, 2000));
    uint64_t t = 2000;
    int beeps = 0;
    /* hovering inside the hysteresis band (-75..-71) keeps warning every 10 s */
    for (int i = 0; i < 15; i++) {
        t += 2000;
        net_warn_action_t a = net_warn_update(&w, &WARN, true, i % 2 ? -72 : -78, t);
        TEST_ASSERT_NOT_EQUAL(NET_WARN_CLEAR, a);
        TEST_ASSERT_NOT_EQUAL(NET_WARN_ENTER, a);
        beeps += a == NET_WARN_REPEAT;
    }
    TEST_ASSERT_EQUAL(3, beeps);                   /* 30 s after the first: at 12, 22, 32 s */
    TEST_ASSERT_EQUAL(NET_WARN_CLEAR, net_warn_update(&w, &WARN, true, -70, t + 2000));
    TEST_ASSERT_FALSE(w.weak);
    TEST_ASSERT_EQUAL(NET_WARN_NONE, net_warn_update(&w, &WARN, true, -74, t + 4000));
}

static void test_warn_lost_link_counts_as_weak(void)
{
    net_warn_t w;
    net_warn_reset(&w);
    net_warn_update(&w, &WARN, true, NET_RSSI_NONE, 0);
    TEST_ASSERT_EQUAL(NET_WARN_ENTER, net_warn_update(&w, &WARN, true, NET_RSSI_NONE, 2000));
    /* no reading is never "recovered" */
    TEST_ASSERT_EQUAL(NET_WARN_NONE, net_warn_update(&w, &WARN, true, NET_RSSI_NONE, 4000));
    TEST_ASSERT_EQUAL(NET_WARN_REPEAT, net_warn_update(&w, &WARN, true, NET_RSSI_NONE, 12000));
}

static void test_warn_disarmed_resets_silently(void)
{
    net_warn_t w;
    net_warn_reset(&w);
    net_warn_update(&w, &WARN, true, -90, 0);
    TEST_ASSERT_EQUAL(NET_WARN_ENTER, net_warn_update(&w, &WARN, true, -90, 2000));
    TEST_ASSERT_EQUAL(NET_WARN_NONE, net_warn_update(&w, &WARN, false, -90, 4000));  /* call over */
    TEST_ASSERT_FALSE(w.weak);
    /* the next call starts over: two samples again */
    TEST_ASSERT_EQUAL(NET_WARN_NONE, net_warn_update(&w, &WARN, true, -90, 6000));
    TEST_ASSERT_EQUAL(NET_WARN_ENTER, net_warn_update(&w, &WARN, true, -90, 8000));
}

static void test_warn_single_sample_config(void)
{
    net_warn_cfg_t c = WARN;
    c.enter_samples = 0;                            /* treated as 1 */
    net_warn_t w;
    net_warn_reset(&w);
    TEST_ASSERT_EQUAL(NET_WARN_ENTER, net_warn_update(&w, &c, true, -76, 0));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_roam_not_before_interval_after_connect);
    RUN_TEST(test_roam_thresholds_idle_vs_call);
    RUN_TEST(test_roam_scan_picks_strongest_other_ap);
    RUN_TEST(test_roam_needs_delta_over_stronger_reading);
    RUN_TEST(test_roam_single_ap_backs_off);
    RUN_TEST(test_roam_empty_scan);
    RUN_TEST(test_warn_enter_needs_consecutive_samples);
    RUN_TEST(test_warn_repeats_and_hysteresis);
    RUN_TEST(test_warn_lost_link_counts_as_weak);
    RUN_TEST(test_warn_disarmed_resets_silently);
    RUN_TEST(test_warn_single_sample_config);
    return UNITY_END();
}
