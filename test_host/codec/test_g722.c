/* Host tests for components/codec G.722 (64 kbit/s): quality on narrowband
 * and wideband tones, silence, state reset, framing invariance, overflow
 * safety, and a known-answer check against Asterisk's codec_g722. */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "g711.h"
#include "g722.h"
#include "resample2.h"
#include "unity.h"
#include "g722_ka.h"

#define FS      16000
#define N       FS                  /* 1 s */
#define FRAME   320                 /* 20 ms @ 16 kHz */

void setUp(void) {}
void tearDown(void) {}

static int16_t s_in[N], s_out[N];
static uint8_t s_code[N / 2];

static void gen_sine(int16_t *x, size_t n, double f, double amp)
{
    for (size_t i = 0; i < n; i++) {
        x[i] = (int16_t)lrint(amp * sin(2.0 * M_PI * f * (double)i / FS));
    }
}

/* Encode and decode frame by frame, like the RTP path does. */
static void roundtrip(const int16_t *in, int16_t *out, size_t n)
{
    g722_enc_t e;
    g722_dec_t d;
    g722_enc_init(&e);
    g722_dec_init(&d);
    for (size_t i = 0; i < n; i += FRAME) {
        TEST_ASSERT_EQUAL_UINT(FRAME / 2, g722_encode(&e, in + i, FRAME, s_code + i / 2));
        TEST_ASSERT_EQUAL_UINT(FRAME, g722_decode(&d, s_code + i / 2, FRAME / 2, out + i));
    }
}

/* SNR of y against x, with y delayed by the best lag in 0..64 (the codec's
 * QMF pair delays the signal). Skips the first 100 ms (adaptation). */
static double snr_db(const int16_t *x, const int16_t *y, size_t n, int *lag_out)
{
    const size_t from = FS / 10, to = n - 100;
    int best = 0;
    double best_c = -1e300;
    for (int lag = 0; lag <= 64; lag++) {
        double c = 0;
        for (size_t i = from; i < to; i++) {
            c += (double)x[i] * y[i + (size_t)lag];
        }
        if (c > best_c) {
            best_c = c;
            best = lag;
        }
    }
    double sig = 0, err = 0;
    for (size_t i = from; i < to; i++) {
        double e = (double)y[i + (size_t)best] - x[i];
        sig += (double)x[i] * x[i];
        err += e * e;
    }
    if (lag_out) {
        *lag_out = best;
    }
    return 10 * log10(sig / (err > 0 ? err : 1e-9));
}

/* Goertzel power, normalised so an amplitude-A sine gives ~A^2/4. */
static double goertzel(const int16_t *x, size_t n, double f)
{
    double w = 2.0 * M_PI * f / FS, c = 2.0 * cos(w), s1 = 0, s2 = 0;
    for (size_t i = 0; i < n; i++) {
        double s0 = x[i] + c * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    return (s1 * s1 + s2 * s2 - c * s1 * s2) / ((double)n * (double)n);
}

void test_framing_sizes(void)
{
    g722_enc_t e;
    g722_dec_t d;
    g722_enc_init(&e);
    g722_dec_init(&d);
    memset(s_in, 0, sizeof(s_in));
    TEST_ASSERT_EQUAL_UINT(160, g722_encode(&e, s_in, 320, s_code));
    TEST_ASSERT_EQUAL_UINT(320, g722_decode(&d, s_code, 160, s_out));
    TEST_ASSERT_EQUAL_UINT(1, g722_encode(&e, s_in, 3, s_code));   /* odd tail ignored */
    TEST_ASSERT_EQUAL_UINT(0, g722_encode(&e, s_in, 0, s_code));
    TEST_ASSERT_EQUAL_UINT(0, g722_decode(&d, s_code, 0, s_out));
}

void test_sine_1khz_snr(void)
{
    gen_sine(s_in, N, 1000, 10000);
    roundtrip(s_in, s_out, N);
    double snr = snr_db(s_in, s_out, N - 64, NULL);
    printf("1 kHz SNR %.1f dB\n", snr);
    TEST_ASSERT_TRUE_MESSAGE(snr > 20, "1 kHz SNR <= 20 dB");
}

void test_sine_5khz_snr_wideband(void)
{
    gen_sine(s_in, N, 5000, 10000);
    roundtrip(s_in, s_out, N);
    double snr = snr_db(s_in, s_out, N - 64, NULL);
    printf("5 kHz SNR %.1f dB\n", snr);
    TEST_ASSERT_TRUE_MESSAGE(snr > 20, "5 kHz SNR <= 20 dB");

    /* For contrast, the narrowband path (16 -> 8 kHz, mu-law, 8 -> 16 kHz)
     * cannot carry 5 kHz at all. */
    static int16_t nb8[N / 2], nb16[N];
    static uint8_t ulaw[N / 2];
    resample2_down_t dn;
    resample2_up_t up;
    resample2_down_init(&dn);
    resample2_up_init(&up);
    resample2_down(&dn, s_in, N / 2, nb8);
    g711_ulaw_encode_buf(nb8, ulaw, N / 2);
    g711_ulaw_decode_buf(ulaw, nb8, N / 2);
    resample2_up(&up, nb8, N / 2, nb16);
    double wb = goertzel(s_out + 1600, N - 1600, 5000);
    double nb = goertzel(nb16 + 1600, N - 1600, 5000);
    TEST_ASSERT_TRUE(10 * log10(wb / goertzel(s_in + 1600, N - 1600, 5000)) > -1.0);
    TEST_ASSERT_TRUE_MESSAGE(10 * log10(nb / wb) < -30, "G.711 path should lose 5 kHz");
}

void test_sine_sweep_snr(void)
{
    /* Low and high sub-band, near the band split and near the top. */
    static const int freqs[] = {300, 2000, 3400, 4500, 6000, 7000};
    for (size_t k = 0; k < sizeof(freqs) / sizeof(freqs[0]); k++) {
        gen_sine(s_in, N, freqs[k], 8000);
        roundtrip(s_in, s_out, N);
        double snr = snr_db(s_in, s_out, N - 64, NULL);
        char msg[64];
        snprintf(msg, sizeof(msg), "%d Hz SNR %.1f dB", freqs[k], snr);
        printf("%s\n", msg);
        TEST_ASSERT_TRUE_MESSAGE(snr > 15, msg);
    }
}

void test_silence_stays_silent(void)
{
    memset(s_in, 0, sizeof(s_in));
    roundtrip(s_in, s_out, N);
    int maxabs = 0;
    for (size_t i = 0; i < N; i++) {
        int a = s_out[i] < 0 ? -s_out[i] : s_out[i];
        if (a > maxabs) {
            maxabs = a;
        }
    }
    TEST_ASSERT_LESS_OR_EQUAL_INT(4, maxabs);

    /* and after loud audio, silence decays back to (near) silence */
    gen_sine(s_in, N / 2, 1500, 20000);
    roundtrip(s_in, s_out, N);
    maxabs = 0;
    for (size_t i = N / 2 + FS / 10; i < N; i++) {
        int a = s_out[i] < 0 ? -s_out[i] : s_out[i];
        if (a > maxabs) {
            maxabs = a;
        }
    }
    TEST_ASSERT_LESS_OR_EQUAL_INT(8, maxabs);
}

void test_state_reset(void)
{
    static uint8_t a[N / 2], b[N / 2];
    static int16_t da[N], db[N];
    g722_enc_t e;
    g722_dec_t d;

    gen_sine(s_in, N, 1234, 12000);
    g722_enc_init(&e);
    g722_encode(&e, s_in, N, a);
    g722_dec_init(&d);
    g722_decode(&d, a, N / 2, da);

    /* Dirty the state with something else, re-init, run again: identical. */
    static int16_t junk[4000];
    for (size_t i = 0; i < 4000; i++) {
        junk[i] = (int16_t)((i * 7919u) % 50000u - 25000);
    }
    g722_encode(&e, junk, 4000, b);
    g722_decode(&d, b, 2000, db);
    g722_enc_init(&e);
    g722_encode(&e, s_in, N, b);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(a, b, N / 2);
    g722_dec_init(&d);
    g722_decode(&d, a, N / 2, db);
    TEST_ASSERT_EQUAL_INT16_ARRAY(da, db, N);
}

void test_chunking_invariant(void)
{
    static uint8_t a[N / 2], b[N / 2];
    static int16_t da[N], db[N];
    g722_enc_t e;
    g722_dec_t d;
    for (size_t i = 0; i < N; i++) {
        s_in[i] = (int16_t)(8000 * sin(i * 0.05) + 3000 * sin(i * 1.3));
    }
    g722_enc_init(&e);
    g722_encode(&e, s_in, N, a);
    g722_enc_init(&e);
    for (size_t i = 0; i < N;) {
        size_t k = (i / 2) % 7 * 2 + 2;            /* uneven even-sized chunks */
        if (k > N - i) {
            k = N - i;
        }
        g722_encode(&e, s_in + i, k, b + i / 2);
        i += k;
    }
    TEST_ASSERT_EQUAL_UINT8_ARRAY(a, b, N / 2);
    g722_dec_init(&d);
    g722_decode(&d, a, N / 2, da);
    g722_dec_init(&d);
    for (size_t i = 0; i < N / 2;) {
        size_t k = i % 5 + 1;
        if (k > N / 2 - i) {
            k = N / 2 - i;
        }
        g722_decode(&d, a + i, k, db + 2 * i);
        i += k;
    }
    TEST_ASSERT_EQUAL_INT16_ARRAY(da, db, N);
}

void test_full_scale_no_overflow(void)
{
    /* Worst cases for the QMF and the predictors: full-scale Nyquist square,
     * full-scale low square, then random codes into the decoder. UBSan
     * catches any signed overflow or bad shift. */
    for (size_t i = 0; i < N; i++) {
        s_in[i] = (i < N / 2) ? ((i & 1) ? INT16_MIN : INT16_MAX) : ((i / 40) & 1 ? INT16_MIN : INT16_MAX);
    }
    roundtrip(s_in, s_out, N);
    g722_dec_t d;
    g722_dec_init(&d);
    uint32_t x = 1;
    for (size_t i = 0; i < N / 2; i++) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        s_code[i] = (uint8_t)x;
    }
    g722_decode(&d, s_code, N / 2, s_out);
}

/* Known answer from Asterisk 20.6's codec_g722 (same SpanDSP algorithm):
 * 2 s of g722_ka_input() (g722_ka.h) converted with Asterisk's own
 * "file convert" sln16 -> g722, and our bitstream g722 -> sln16. The
 * expected values are FNV-1a hashes of Asterisk's output files;
 * test_host/sip/integration.sh repeats the comparison live (cmp). The ITU
 * G.722 test vectors are not part of this repo. */
static uint32_t fnv1a(const void *p, size_t n)
{
    const uint8_t *b = p;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) {
        h = (h ^ b[i]) * 16777619u;
    }
    return h;
}

#define KA_N            (2 * FS)
#define KA_ENC_FNV      0x0F51476Au     /* Asterisk 20.6 codec_g722 output */
#define KA_DEC_FNV      0x5A8335C4u

void test_known_answer_vs_asterisk(void)
{
    static int16_t x[KA_N], y[KA_N];
    static uint8_t c[KA_N / 2];
    g722_enc_t e;
    g722_dec_t d;
    g722_ka_input(x, KA_N);
    g722_enc_init(&e);
    g722_encode(&e, x, KA_N, c);
    g722_dec_init(&d);
    g722_decode(&d, c, KA_N / 2, y);
    TEST_ASSERT_EQUAL_HEX32(KA_ENC_FNV, fnv1a(c, sizeof(c)));
    TEST_ASSERT_EQUAL_HEX32(KA_DEC_FNV, fnv1a(y, sizeof(y)));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_framing_sizes);
    RUN_TEST(test_sine_1khz_snr);
    RUN_TEST(test_sine_5khz_snr_wideband);
    RUN_TEST(test_sine_sweep_snr);
    RUN_TEST(test_silence_stays_silent);
    RUN_TEST(test_state_reset);
    RUN_TEST(test_chunking_invariant);
    RUN_TEST(test_full_scale_no_overflow);
    RUN_TEST(test_known_answer_vs_asterisk);
    return UNITY_END();
}
