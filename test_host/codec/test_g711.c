/* Host tests for components/codec (G.711 mu-law / A-law). */

#include <stdint.h>
#include "g711.h"
#include "unity.h"

/* One period of a 1 kHz sine at 8 kHz, amplitude 16000 (8 samples/period). */
static const int16_t s_sine8[8] = {0, 11314, 16000, 11314, 0, -11314, -16000, -11314};

static int16_t s_pcm_all[65536];
static uint8_t s_code_buf[65536];
static int16_t s_dec_buf[65536];

void setUp(void) {}
void tearDown(void) {}

void test_ulaw_roundtrip_all_codes(void)
{
    for (int c = 0; c < 256; c++) {
        uint8_t code = (uint8_t)c;
        uint8_t back = g711_ulaw_encode(g711_ulaw_decode(code));
        /* 0x7F and 0xFF both decode to 0; encode(0) is 0xFF. */
        if (code == 0x7F) {
            TEST_ASSERT_EQUAL_HEX8(0xFF, back);
        } else {
            TEST_ASSERT_EQUAL_HEX8(code, back);
        }
    }
}

void test_alaw_roundtrip_all_codes(void)
{
    for (int c = 0; c < 256; c++) {
        uint8_t code = (uint8_t)c;
        TEST_ASSERT_EQUAL_HEX8(code, g711_alaw_encode(g711_alaw_decode(code)));
    }
}

void test_ulaw_known_values(void)
{
    TEST_ASSERT_EQUAL_HEX8(0xFF, g711_ulaw_encode(0));
    TEST_ASSERT_EQUAL_INT16(-32124, g711_ulaw_decode(0x00));
    TEST_ASSERT_EQUAL_INT16(32124, g711_ulaw_decode(0x80));
    TEST_ASSERT_EQUAL_INT16(0, g711_ulaw_decode(0x7F));
    TEST_ASSERT_EQUAL_INT16(0, g711_ulaw_decode(0xFF));
}

void test_alaw_known_values(void)
{
    TEST_ASSERT_EQUAL_HEX8(0xD5, g711_alaw_encode(0));
    TEST_ASSERT_EQUAL_INT16(8, g711_alaw_decode(0xD5));
    TEST_ASSERT_EQUAL_INT16(-8, g711_alaw_decode(0x55));
}

void test_ulaw_decode_monotonic(void)
{
    /* Positive half is codes 0x80..0xFF, decreasing in value as code rises. */
    for (int c = 0x80; c < 0xFF; c++) {
        TEST_ASSERT_TRUE_MESSAGE(g711_ulaw_decode((uint8_t)c) > g711_ulaw_decode((uint8_t)(c + 1)),
                                 "mu-law positive half not monotonic");
    }
    /* Negative half is codes 0x00..0x7F, increasing up to 0. */
    for (int c = 0x00; c < 0x7F; c++) {
        TEST_ASSERT_TRUE_MESSAGE(g711_ulaw_decode((uint8_t)c) < g711_ulaw_decode((uint8_t)(c + 1)),
                                 "mu-law negative half not monotonic");
    }
}

void test_alaw_decode_monotonic(void)
{
    /* Magnitude is (code ^ 0x55) low 7 bits order, so walk magnitude m = 0..126. */
    for (int m = 0; m < 127; m++) {
        uint8_t pos_lo = (uint8_t)((m | 0x80) ^ 0x55);
        uint8_t pos_hi = (uint8_t)(((m + 1) | 0x80) ^ 0x55);
        uint8_t neg_lo = (uint8_t)(m ^ 0x55);
        uint8_t neg_hi = (uint8_t)((m + 1) ^ 0x55);
        TEST_ASSERT_TRUE_MESSAGE(g711_alaw_decode(pos_lo) < g711_alaw_decode(pos_hi),
                                 "A-law positive half not monotonic");
        TEST_ASSERT_TRUE_MESSAGE(g711_alaw_decode(neg_lo) > g711_alaw_decode(neg_hi),
                                 "A-law negative half not monotonic");
    }
}

void test_encode_is_monotonic_over_full_range(void)
{
    int16_t prev_u = g711_ulaw_decode(g711_ulaw_encode(INT16_MIN));
    int16_t prev_a = g711_alaw_decode(g711_alaw_encode(INT16_MIN));
    for (int x = INT16_MIN + 1; x <= INT16_MAX; x++) {
        int16_t cur_u = g711_ulaw_decode(g711_ulaw_encode((int16_t)x));
        int16_t cur_a = g711_alaw_decode(g711_alaw_encode((int16_t)x));
        TEST_ASSERT_TRUE_MESSAGE(cur_u >= prev_u, "mu-law encode not monotonic");
        TEST_ASSERT_TRUE_MESSAGE(cur_a >= prev_a, "A-law encode not monotonic");
        prev_u = cur_u;
        prev_a = cur_a;
    }
}

void test_saturation(void)
{
    TEST_ASSERT_EQUAL_HEX8(0x80, g711_ulaw_encode(INT16_MAX));
    TEST_ASSERT_EQUAL_HEX8(0x00, g711_ulaw_encode(INT16_MIN));
    TEST_ASSERT_EQUAL_INT16(32124, g711_ulaw_decode(g711_ulaw_encode(INT16_MAX)));
    TEST_ASSERT_EQUAL_INT16(-32124, g711_ulaw_decode(g711_ulaw_encode(INT16_MIN)));

    TEST_ASSERT_EQUAL_HEX8(0xAA, g711_alaw_encode(INT16_MAX));
    TEST_ASSERT_EQUAL_HEX8(0x2A, g711_alaw_encode(INT16_MIN));
    TEST_ASSERT_EQUAL_INT16(32256, g711_alaw_decode(g711_alaw_encode(INT16_MAX)));
    TEST_ASSERT_EQUAL_INT16(-32256, g711_alaw_decode(g711_alaw_encode(INT16_MIN)));
}

void test_sine_snr(void)
{
    /* SNR > 35 dB  <=>  signal / noise > 10^3.5 = 3162.28 */
    const double min_ratio = 3162.28;
    const int n = 800;
    double sig_u = 0.0, noise_u = 0.0;
    double sig_a = 0.0, noise_a = 0.0;

    for (int i = 0; i < n; i++) {
        int16_t x = s_sine8[i % 8];
        int16_t yu = g711_ulaw_decode(g711_ulaw_encode(x));
        int16_t ya = g711_alaw_decode(g711_alaw_encode(x));
        double du = (double)x - (double)yu;
        double da = (double)x - (double)ya;
        sig_u += (double)x * (double)x;
        sig_a += (double)x * (double)x;
        noise_u += du * du;
        noise_a += da * da;
    }
    TEST_ASSERT_TRUE_MESSAGE(sig_u > min_ratio * noise_u, "mu-law SNR below 35 dB");
    TEST_ASSERT_TRUE_MESSAGE(sig_a > min_ratio * noise_a, "A-law SNR below 35 dB");
}

void test_buffers_match_scalar(void)
{
    for (int i = 0; i < 65536; i++) {
        s_pcm_all[i] = (int16_t)(i - 32768);
    }

    g711_ulaw_encode_buf(s_pcm_all, s_code_buf, 65536);
    for (int i = 0; i < 65536; i++) {
        TEST_ASSERT_EQUAL_HEX8(g711_ulaw_encode(s_pcm_all[i]), s_code_buf[i]);
    }
    g711_ulaw_decode_buf(s_code_buf, s_dec_buf, 65536);
    for (int i = 0; i < 65536; i++) {
        TEST_ASSERT_EQUAL_INT16(g711_ulaw_decode(s_code_buf[i]), s_dec_buf[i]);
    }

    g711_alaw_encode_buf(s_pcm_all, s_code_buf, 65536);
    for (int i = 0; i < 65536; i++) {
        TEST_ASSERT_EQUAL_HEX8(g711_alaw_encode(s_pcm_all[i]), s_code_buf[i]);
    }
    g711_alaw_decode_buf(s_code_buf, s_dec_buf, 65536);
    for (int i = 0; i < 65536; i++) {
        TEST_ASSERT_EQUAL_INT16(g711_alaw_decode(s_code_buf[i]), s_dec_buf[i]);
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_ulaw_roundtrip_all_codes);
    RUN_TEST(test_alaw_roundtrip_all_codes);
    RUN_TEST(test_ulaw_known_values);
    RUN_TEST(test_alaw_known_values);
    RUN_TEST(test_ulaw_decode_monotonic);
    RUN_TEST(test_alaw_decode_monotonic);
    RUN_TEST(test_encode_is_monotonic_over_full_range);
    RUN_TEST(test_saturation);
    RUN_TEST(test_sine_snr);
    RUN_TEST(test_buffers_match_scalar);
    return UNITY_END();
}
