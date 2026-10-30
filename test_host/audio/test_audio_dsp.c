#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "unity.h"
#include "audio_dsp.h"

#define FS16 16000

void setUp(void) {}
void tearDown(void) {}

/* ---- helpers ---- */

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

/* Goertzel power (normalised so a full-scale-amplitude A sine gives ~A^2/4). */
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

static void tone_render(dsp_tone_t *t, int16_t *out, size_t n)
{
    int32_t acc[160];
    size_t i = 0;
    while (i < n) {
        size_t k = n - i > 160 ? 160 : n - i;
        memset(acc, 0, sizeof(acc));
        dsp_tone_add(t, acc, k);
        for (size_t j = 0; j < k; j++) {
            TEST_ASSERT_TRUE(acc[j] >= INT16_MIN && acc[j] <= INT16_MAX);
            out[i + j] = (int16_t)acc[j];
        }
        i += k;
    }
}

/* ---- tone generator ---- */

static void test_tone_single_frequency(void)
{
    enum { N = FS16 };       /* 1 s */
    static int16_t buf[N];
    dsp_tone_t t;
    dsp_tone_def_t def = { .f1 = 1000 };

    dsp_tone_init(&t, FS16);
    dsp_tone_start_def(&t, &def);
    tone_render(&t, buf, N);

    int crossings = 0;
    for (int i = 1; i < N; i++) {
        if ((buf[i - 1] < 0) != (buf[i] < 0)) {
            crossings++;
        }
    }
    /* 1 kHz over 1 s -> 2000 zero crossings (a few off for the attack ramp) */
    TEST_ASSERT_INT_WITHIN(3, 2000, crossings);
    TEST_ASSERT_TRUE(rms16(buf, N) > 3000);
}

static void test_tone_dial_components(void)
{
    enum { N = FS16 };
    static int16_t buf[N];
    dsp_tone_t t;

    dsp_tone_init(&t, FS16);
    dsp_tone_start(&t, DSP_TONE_DIAL);
    tone_render(&t, buf, N);

    double p350 = goertzel(buf, N, 350, FS16), p440 = goertzel(buf, N, 440, FS16);
    double pbad = goertzel(buf, N, 600, FS16);
    TEST_ASSERT_TRUE(p350 > 1e5 && p440 > 1e5);
    TEST_ASSERT_TRUE(10 * log10(p350 / pbad) > 40);
    TEST_ASSERT_TRUE(dsp_tone_active(&t));      /* continuous */
}

/* Returns the on/off run lengths (ms) found by 10 ms block envelope. */
static int find_runs(dsp_tone_kind_t kind, int total_ms, int *runs, int max_runs)
{
    size_t n = (size_t)total_ms * FS16 / 1000;
    int16_t *buf = malloc(n * sizeof(int16_t));
    dsp_tone_t t;
    int nruns = 0, cur_len = 0;
    bool cur = false, first = true;

    TEST_ASSERT_NOT_NULL(buf);
    dsp_tone_init(&t, FS16);
    dsp_tone_start(&t, kind);
    tone_render(&t, buf, n);

    const size_t blk = FS16 / 100;                      /* 10 ms blocks */
    for (size_t b = 0; b + blk <= n; b += blk) {
        int peak = 0;
        for (size_t i = b; i < b + blk; i++) {
            int a = abs(buf[i]);
            if (a > peak) {
                peak = a;
            }
        }
        bool on = peak > 2000;
        if (first) {
            cur = on;
            first = false;
        }
        if (on == cur) {
            cur_len += 10;
        } else {
            if (nruns < max_runs) {
                runs[nruns] = cur ? cur_len : -cur_len;
            }
            nruns++;
            cur = on;
            cur_len = 10;
        }
    }
    free(buf);
    return nruns;
}

static void test_ringtones_sound_and_pause(void)
{
    TEST_ASSERT_GREATER_OR_EQUAL_INT(5, dsp_ringtone_count());
    TEST_ASSERT_NULL(dsp_ringtone_name(dsp_ringtone_count()));
    size_t n = (size_t)6000 * FS16 / 1000;
    int32_t *acc = malloc(n * sizeof(int32_t));
    TEST_ASSERT_NOT_NULL(acc);
    for (int i = -1; i <= dsp_ringtone_count(); i++) {      /* -1 and count: fall back to 0 */
        dsp_tone_t t;
        dsp_tone_init(&t, FS16);
        dsp_tone_start_ringtone(&t, i);
        memset(acc, 0, n * sizeof(int32_t));
        dsp_tone_add(&t, acc, n);
        int on = 0, off = 0, peak = 0;
        for (size_t b = 0; b + 160 <= n; b += 160) {         /* 10 ms blocks */
            int p = 0;
            for (size_t k = b; k < b + 160; k++) {
                int a = abs((int)acc[k]);
                p = a > p ? a : p;
            }
            peak = p > peak ? p : peak;
            if (p > 2000) {
                on++;
            } else {
                off++;
            }
        }
        TEST_ASSERT_GREATER_THAN_INT(20, on);                /* audible */
        TEST_ASSERT_GREATER_THAN_INT(100, off);              /* with a pause */
        TEST_ASSERT_LESS_THAN_INT(32768, peak);
        TEST_ASSERT_TRUE(dsp_tone_active(&t));
    }
    free(acc);
}

static void test_tone_cadence_ringback(void)
{
    int runs[8];
    int n = find_runs(DSP_TONE_RINGBACK, 14000, runs, 8);   /* on 2 / off 4 / on 2 / off 4 / on 2 */
    TEST_ASSERT_TRUE(n >= 4);
    TEST_ASSERT_INT_WITHIN(20, 2000, runs[0]);
    TEST_ASSERT_INT_WITHIN(20, -4000, runs[1]);
    TEST_ASSERT_INT_WITHIN(20, 2000, runs[2]);
    TEST_ASSERT_INT_WITHIN(20, -4000, runs[3]);
}

static void test_tone_cadence_busy(void)
{
    int runs[8];
    int n = find_runs(DSP_TONE_BUSY, 4000, runs, 8);
    TEST_ASSERT_TRUE(n >= 4);
    for (int i = 0; i < 4; i++) {
        TEST_ASSERT_INT_WITHIN(20, (i & 1) ? -500 : 500, runs[i]);
    }
}

static void test_tone_cadence_ring(void)
{
    int runs[8];
    int n = find_runs(DSP_TONE_RING, 8000, runs, 8);        /* 1 s on / 2 s off */
    TEST_ASSERT_TRUE(n >= 3);
    TEST_ASSERT_INT_WITHIN(20, 1000, runs[0]);
    TEST_ASSERT_INT_WITHIN(20, -2000, runs[1]);
    TEST_ASSERT_INT_WITHIN(20, 1000, runs[2]);
}

static void test_tone_ring_warbles(void)
{
    enum { N = FS16 / 2 };
    static int16_t buf[N];
    dsp_tone_t t;

    dsp_tone_init(&t, FS16);
    dsp_tone_start(&t, DSP_TONE_RING);
    tone_render(&t, buf, N);
    /* energy at both warble frequencies */
    TEST_ASSERT_TRUE(goertzel(buf, N, 1000, FS16) > 1e4);
    TEST_ASSERT_TRUE(goertzel(buf, N, 1200, FS16) > 1e4);
}

static void test_tone_keyclick_oneshot(void)
{
    static int16_t buf[FS16];
    dsp_tone_t t;
    dsp_tone_init(&t, FS16);
    dsp_tone_start(&t, DSP_TONE_KEYCLICK);
    tone_render(&t, buf, FS16 / 10);                         /* 100 ms */

    size_t last = 0;
    for (size_t i = 0; i < FS16 / 10; i++) {
        if (abs(buf[i]) > 100) {
            last = i;
        }
    }
    /* ~30 ms + 5 ms release */
    TEST_ASSERT_TRUE(last > 20 * FS16 / 1000);
    TEST_ASSERT_TRUE(last < 45 * FS16 / 1000);
    TEST_ASSERT_FALSE(dsp_tone_active(&t));
}

/* Counted one-shot (weak-signal warning): exactly two bursts, then inactive. */
static void test_tone_weak_signal_two_bursts(void)
{
    static int16_t buf[FS16 / 2];
    dsp_tone_t t;
    dsp_tone_init(&t, FS16);
    dsp_tone_start(&t, DSP_TONE_WEAK_SIGNAL);
    tone_render(&t, buf, FS16 / 2);                          /* 500 ms */

    int bursts = 0;
    size_t last = 0, quiet = 0;
    bool on = false;
    for (size_t i = 0; i < FS16 / 2; i++) {
        bool loud = abs(buf[i]) > 100;
        if (loud) {
            last = i;
            quiet = 0;
            if (!on) {
                bursts++;
                on = true;
            }
        } else if (on && ++quiet > 16) {                    /* > 1 ms of silence */
            on = false;
        }
    }
    TEST_ASSERT_EQUAL(2, bursts);
    /* second burst: 150 ms period + 70 ms on + 5 ms release */
    TEST_ASSERT_TRUE(last > 200 * FS16 / 1000);
    TEST_ASSERT_TRUE(last < 230 * FS16 / 1000);
    TEST_ASSERT_FALSE(dsp_tone_active(&t));
}

static void test_tone_stop_releases(void)
{
    static int16_t buf[FS16 / 10];
    dsp_tone_t t;
    dsp_tone_init(&t, FS16);
    dsp_tone_start(&t, DSP_TONE_DIAL);
    tone_render(&t, buf, 800);
    dsp_tone_stop(&t);
    tone_render(&t, buf, 800);
    TEST_ASSERT_FALSE(dsp_tone_active(&t));
    TEST_ASSERT_EQUAL_INT16(0, buf[799]);
    /* released smoothly: no jump beyond the max slope of the 350+440 Hz pair (~6100) */
    for (int i = 1; i < 800; i++) {
        TEST_ASSERT_TRUE(abs(buf[i] - buf[i - 1]) < 7000);
    }
}

static void test_dtmf_pairs(void)
{
    static const char keys[] = "123A456B789C*0#D";
    static const unsigned row[4] = { 697, 770, 852, 941 };
    static const unsigned col[4] = { 1209, 1336, 1477, 1633 };
    enum { N = FS16 / 10 };
    static int16_t buf[N];

    for (int k = 0; k < 16; k++) {
        dsp_tone_t t;
        dsp_tone_init(&t, FS16);
        TEST_ASSERT_TRUE(dsp_tone_start_dtmf(&t, keys[k]));
        tone_render(&t, buf, N);

        unsigned want_lo = row[k / 4], want_hi = col[k % 4];
        unsigned lo, hi;
        TEST_ASSERT_TRUE(dsp_dtmf_freqs(keys[k], &lo, &hi));
        TEST_ASSERT_EQUAL_UINT(want_lo, lo);
        TEST_ASSERT_EQUAL_UINT(want_hi, hi);

        double p_lo = goertzel(buf, N, want_lo, FS16), p_hi = goertzel(buf, N, want_hi, FS16);
        double worst = 0;
        for (int i = 0; i < 4; i++) {
            if (row[i] != want_lo) {
                double p = goertzel(buf, N, row[i], FS16);
                worst = p > worst ? p : worst;
            }
            if (col[i] != want_hi) {
                double p = goertzel(buf, N, col[i], FS16);
                worst = p > worst ? p : worst;
            }
        }
        TEST_ASSERT_TRUE_MESSAGE(10 * log10(p_lo / worst) > 20, "row tone not dominant");
        TEST_ASSERT_TRUE_MESSAGE(10 * log10(p_hi / worst) > 20, "col tone not dominant");
        /* twist: tones are generated at equal level */
        TEST_ASSERT_TRUE(fabs(10 * log10(p_lo / p_hi)) < 1.0);
    }
    dsp_tone_t t;
    dsp_tone_init(&t, FS16);
    TEST_ASSERT_FALSE(dsp_tone_start_dtmf(&t, 'x'));
    TEST_ASSERT_FALSE(dsp_tone_active(&t));
}

static void test_dtmf_duration(void)
{
    static int16_t buf[FS16];
    dsp_tone_t t;
    dsp_tone_init(&t, FS16);
    dsp_tone_start_dtmf(&t, '5');
    tone_render(&t, buf, FS16 / 2);
    size_t last = 0;
    for (size_t i = 0; i < FS16 / 2; i++) {
        if (abs(buf[i]) > 100) {
            last = i;
        }
    }
    TEST_ASSERT_INT_WITHIN(8 * FS16 / 1000, 105 * FS16 / 1000, (int)last);   /* 100 ms + release */
    TEST_ASSERT_FALSE(dsp_tone_active(&t));
}

/* ---- DC blocker ---- */

static void test_dcblock_removes_offset(void)
{
    enum { N = FS16 * 2 };
    static int32_t x[N];
    static int16_t y[N];
    dsp_dcblock_t s;

    for (int i = 0; i < N; i++) {
        x[i] = 5000;
    }
    dsp_dcblock_init(&s, FS16, 20);
    dsp_dcblock_process(&s, x, y, N);
    for (int i = FS16; i < N; i++) {
        TEST_ASSERT_INT_WITHIN(2, 0, y[i]);
    }
}

static void test_dcblock_passes_voice(void)
{
    enum { N = FS16 };
    static int16_t t[N], y[N];
    static int32_t x[N];
    dsp_dcblock_t s;

    gen_sine(t, N, 1000, FS16, 8000);
    for (int i = 0; i < N; i++) {
        x[i] = t[i] + 3000;                     /* tone riding on a DC offset */
    }
    dsp_dcblock_init(&s, FS16, 20);
    dsp_dcblock_process(&s, x, y, N);
    double gain_db = 20 * log10(rms16(y + 8000, N - 8000) / rms16(t + 8000, N - 8000));
    TEST_ASSERT_TRUE(fabs(gain_db) < 0.5);
}

static void test_dcblock_attenuates_below_cutoff(void)
{
    enum { N = FS16 * 2 };
    static int16_t t[N], y[N];
    static int32_t x[N];
    dsp_dcblock_t s;

    gen_sine(t, N, 5, FS16, 8000);
    for (int i = 0; i < N; i++) {
        x[i] = t[i];
    }
    dsp_dcblock_init(&s, FS16, 20);
    dsp_dcblock_process(&s, x, y, N);
    double gain_db = 20 * log10(rms16(y + FS16, N - FS16) / rms16(t + FS16, N - FS16));
    TEST_ASSERT_TRUE(gain_db < -9);
}

static void test_dcblock_saturates(void)
{
    dsp_dcblock_t s;
    dsp_dcblock_init(&s, FS16, 20);
    TEST_ASSERT_EQUAL_INT16(INT16_MAX, dsp_dcblock_step(&s, 1 << 20));
    dsp_dcblock_init(&s, FS16, 20);
    TEST_ASSERT_EQUAL_INT16(INT16_MIN, dsp_dcblock_step(&s, -(1 << 20)));
}

/* ---- gain + soft clip ---- */

static void test_gain_curve(void)
{
    TEST_ASSERT_EQUAL_INT32(0, dsp_gain_q15(0));
    TEST_ASSERT_EQUAL_INT32(DSP_GAIN_UNITY, dsp_gain_q15(100));
    TEST_ASSERT_EQUAL_INT32(DSP_GAIN_UNITY, dsp_gain_q15(250));     /* clamped */
    TEST_ASSERT_INT_WITHIN(2, DSP_GAIN_UNITY / 4, dsp_gain_q15(50));
    int32_t prev = -1;
    for (int v = 0; v <= 100; v++) {
        int32_t g = dsp_gain_q15((uint8_t)v);
        TEST_ASSERT_TRUE(g > prev || v == 0);
        prev = g;
    }
}

static void test_gain_zero_silence_and_unity(void)
{
    int32_t a[160], b[160];
    for (int i = 0; i < 160; i++) {
        a[i] = b[i] = (i * 397) % 60000 - 30000;
    }
    dsp_gain_ramp(a, 160, 0, 0);
    for (int i = 0; i < 160; i++) {
        TEST_ASSERT_EQUAL_INT32(0, a[i]);
    }
    int32_t g = dsp_gain_q15(100);
    dsp_gain_ramp(b, 160, g, g);
    for (int i = 0; i < 160; i++) {
        TEST_ASSERT_EQUAL_INT32((i * 397) % 60000 - 30000, b[i]);
    }
}

static void test_gain_ramp_monotonic(void)
{
    int32_t a[160];
    for (int i = 0; i < 160; i++) {
        a[i] = 10000;
    }
    dsp_gain_ramp(a, 160, 0, DSP_GAIN_UNITY);
    for (int i = 1; i < 160; i++) {
        TEST_ASSERT_TRUE(a[i] >= a[i - 1]);
    }
    TEST_ASSERT_EQUAL_INT32(10000, a[159]);
    TEST_ASSERT_TRUE(a[0] < 200);
}

static void test_softclip_never_wraps(void)
{
    static const int32_t probes[] = {
        0, 1, -1, 24576, 24577, -24577, 32767, 32768, -32768, 40000, -40000,
        65535, -65536, 1 << 20, -(1 << 20), INT32_MAX, INT32_MIN,
    };
    for (size_t i = 0; i < sizeof(probes) / sizeof(probes[0]); i++) {
        int32_t x = probes[i];
        int16_t y = dsp_softclip(x);
        TEST_ASSERT_TRUE(y >= -32766 && y <= 32766);
        if (x > 0) {
            TEST_ASSERT_TRUE(y > 0);
        }
        if (x < 0) {
            TEST_ASSERT_TRUE(y < 0);
        }
        if (x != INT32_MIN) {
            TEST_ASSERT_EQUAL_INT16(-y, dsp_softclip(-x));      /* odd symmetry */
        }
    }
    /* sweep: monotonic, linear below the knee */
    int16_t prev = dsp_softclip(-100000);
    for (int32_t x = -100000; x <= 100000; x += 7) {
        int16_t y = dsp_softclip(x);
        TEST_ASSERT_TRUE(y >= prev);
        prev = y;
        if (abs(x) <= 24576) {
            TEST_ASSERT_EQUAL_INT16(x, y);
        }
    }
}

static void test_softclip_buf(void)
{
    int32_t in[4] = { 100, -100, 50000, -50000 };
    int16_t out[4];
    dsp_softclip_buf(in, out, 4);
    TEST_ASSERT_EQUAL_INT16(100, out[0]);
    TEST_ASSERT_EQUAL_INT16(-100, out[1]);
    TEST_ASSERT_TRUE(out[2] > 24576 && out[2] < 32767);
    TEST_ASSERT_EQUAL_INT16(-out[2], out[3]);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_tone_single_frequency);
    RUN_TEST(test_tone_dial_components);
    RUN_TEST(test_ringtones_sound_and_pause);
    RUN_TEST(test_tone_cadence_ringback);
    RUN_TEST(test_tone_cadence_busy);
    RUN_TEST(test_tone_cadence_ring);
    RUN_TEST(test_tone_ring_warbles);
    RUN_TEST(test_tone_keyclick_oneshot);
    RUN_TEST(test_tone_weak_signal_two_bursts);
    RUN_TEST(test_tone_stop_releases);
    RUN_TEST(test_dtmf_pairs);
    RUN_TEST(test_dtmf_duration);
    RUN_TEST(test_dcblock_removes_offset);
    RUN_TEST(test_dcblock_passes_voice);
    RUN_TEST(test_dcblock_attenuates_below_cutoff);
    RUN_TEST(test_dcblock_saturates);
    RUN_TEST(test_gain_curve);
    RUN_TEST(test_gain_zero_silence_and_unity);
    RUN_TEST(test_gain_ramp_monotonic);
    RUN_TEST(test_softclip_never_wraps);
    RUN_TEST(test_softclip_buf);
    return UNITY_END();
}
