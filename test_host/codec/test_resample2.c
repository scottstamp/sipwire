/* Host tests for components/codec resample2 (half-band 2:1 / 1:2). */

#include <math.h>
#include <stdint.h>
#include <string.h>

#include "resample2.h"
#include "unity.h"

#define FS8  8000
#define FS16 16000

void setUp(void) {}
void tearDown(void) {}

static double rms16(const int16_t *x, size_t n)
{
    double acc = 0;
    for (size_t i = 0; i < n; i++) {
        acc += (double)x[i] * x[i];
    }
    return sqrt(acc / (double)n);
}

static void gen_sine(int16_t *x, size_t n, double f, double fs, double amp)
{
    for (size_t i = 0; i < n; i++) {
        x[i] = (int16_t)lrint(amp * sin(2.0 * M_PI * f * (double)i / fs));
    }
}

static double goertzel(const int16_t *x, size_t n, double f, double fs)
{
    double w = 2.0 * M_PI * f / fs, c = 2.0 * cos(w), s1 = 0, s2 = 0;
    for (size_t i = 0; i < n; i++) {
        double s0 = x[i] + c * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    double p = s1 * s1 + s2 * s2 - c * s1 * s2;
    return p / ((double)n * (double)n);
}

static void test_passband_round_trip(void)
{
    enum { N = FS8 };
    static int16_t x[N], up[2 * N], y[N];
    resample2_up_t u;
    resample2_down_t d;

    gen_sine(x, N, 1000, FS8, 10000);
    resample2_up_init(&u);
    resample2_down_init(&d);
    /* process in 20 ms frames like the RTP path */
    for (int i = 0; i < N; i += 160) {
        resample2_up(&u, x + i, 160, up + 2 * i);
        resample2_down(&d, up + 2 * i, 160, y + i);
    }
    double gain_db = 20 * log10(rms16(y + 400, N - 400) / rms16(x + 400, N - 400));
    TEST_ASSERT_TRUE_MESSAGE(fabs(gain_db) < 1.0, "1 kHz not preserved within 1 dB");
}

static void test_up_is_clean(void)
{
    enum { N = FS8 };
    static int16_t x[N], up[2 * N];
    resample2_up_t u;

    gen_sine(x, N, 1000, FS8, 10000);
    resample2_up_init(&u);
    resample2_up(&u, x, N, up);
    /* image at 8k-1k = 7k must be strongly suppressed */
    double fund = goertzel(up + 800, N, 1000, FS16);
    double img = goertzel(up + 800, N, 7000, FS16);
    TEST_ASSERT_TRUE(10 * log10(fund / img) > 40);
    TEST_ASSERT_TRUE(fabs(20 * log10(rms16(up + 800, N) / rms16(x + 400, N - 400))) < 1.0);
}

static void test_down_stopband(void)
{
    enum { N = FS16 };
    static int16_t x[N], y[N / 2];
    resample2_down_t d;

    gen_sine(x, N, 6000, FS16, 10000);
    resample2_down_init(&d);
    resample2_down(&d, x, N / 2, y);
    double att_db = 20 * log10(rms16(y + 100, N / 2 - 100) / rms16(x + 200, N - 200));
    TEST_ASSERT_TRUE_MESSAGE(att_db < -30, "6 kHz alias not attenuated > 30 dB");
}

static void test_chunking_invariant(void)
{
    enum { N = 1000 };
    static int16_t x[2 * N], a[N], b[N], ua[4 * N], ub[4 * N];
    resample2_down_t d1, d2;
    resample2_up_t u1, u2;

    for (int i = 0; i < 2 * N; i++) {
        x[i] = (int16_t)((i * 7919) % 20000 - 10000);
    }
    resample2_down_init(&d1);
    resample2_down_init(&d2);
    resample2_down(&d1, x, N, a);
    for (int i = 0; i < N; i += 160) {
        int k = N - i > 160 ? 160 : N - i;
        resample2_down(&d2, x + 2 * i, (size_t)k, b + i);
    }
    TEST_ASSERT_EQUAL_INT16_ARRAY(a, b, N);

    resample2_up_init(&u1);
    resample2_up_init(&u2);
    resample2_up(&u1, x, 2 * N, ua);                /* > internal block size */
    for (int i = 0; i < 2 * N; i += 37) {
        int k = 2 * N - i > 37 ? 37 : 2 * N - i;
        resample2_up(&u2, x + i, (size_t)k, ub + 2 * i);
    }
    TEST_ASSERT_EQUAL_INT16_ARRAY(ua, ub, 4 * N);
}

static void test_full_scale_no_wrap(void)
{
    enum { N = 800 };
    static int16_t x[N], up[2 * N], y[N];
    resample2_up_t u;
    resample2_down_t d;
    resample2_up_init(&u);
    for (int i = 0; i < N; i++) {
        x[i] = (i & 1) ? INT16_MIN : INT16_MAX;     /* worst case: Nyquist square */
    }
    resample2_up(&u, x, N, up);
    resample2_down_init(&d);
    resample2_down(&d, up, N, y);
    (void)y;                                        /* ASan/UBSan: no overflow */
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_passband_round_trip);
    RUN_TEST(test_up_is_clean);
    RUN_TEST(test_down_stopband);
    RUN_TEST(test_chunking_invariant);
    RUN_TEST(test_full_scale_no_wrap);
    return UNITY_END();
}
