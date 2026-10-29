/* Echo canceller (components/aec) on synthetic signals that mimic the device:
 * the speaker stream goes through the reference ring exactly like tx_write ->
 * audio_aec_rx, and the mic hears it D + misalignment samples later through a
 * short enclosure/room impulse response, plus a little noise. */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "unity.h"
#include "aec.h"

#define FS          AEC_SAMPLE_RATE
#define FRAME       AEC_FRAME_SAMPLES
#define DELAY       640                 /* nominal speaker->mic offset (audio.c: TX DMA depth) */
#define RING        4096
#define IR_LEN      560                 /* 35 ms: inside the default tail minus the lead */
#define PI_F        3.14159265f

void setUp(void) {}
void tearDown(void) {}

/* ------------------------------------------------------------------ */
/* signals                                                            */
/* ------------------------------------------------------------------ */

static float rnd(uint32_t *s)
{
    *s = *s * 1664525u + 1013904223u;
    return (float)((int32_t)*s >> 8) / 8388608.0f;          /* [-1, 1) */
}

/* Speech-like: white noise through a 150 Hz high-pass and two 1.5 kHz
 * one-pole low-passes (speech-ish spectral tilt), a ~4 Hz syllable envelope,
 * and talk spurts of 0.8-2 s separated by 0.3-0.9 s pauses. rms: during
 * spurts, before the envelope. */
static void gen_speech(int16_t *x, size_t n, uint32_t seed, float rms, size_t start)
{
    uint32_t s = seed;
    float hp_x = 0, hp_y = 0, lp1 = 0, lp2 = 0;
    const float a_hp = expf(-2.0f * PI_F * 150.0f / FS);
    const float a_lp = expf(-2.0f * PI_F * 1500.0f / FS);
    size_t next = start;
    bool on = false;
    float syl_f = 4.0f, ph = 0;
    const float gain = rms * 3.0f;          /* roughly compensates the filters' loss */

    for (size_t i = 0; i < n; i++) {
        if (i >= next) {
            on = !on;
            float u = (rnd(&s) + 1.0f) * 0.5f;
            next = i + (size_t)((on ? 0.8f + 1.2f * u : 0.3f + 0.6f * u) * FS);
            syl_f = 3.0f + 2.0f * u;
        }
        float w = rnd(&s);
        hp_y = a_hp * (hp_y + w - hp_x);
        hp_x = w;
        lp1 = a_lp * lp1 + (1.0f - a_lp) * hp_y;
        lp2 = a_lp * lp2 + (1.0f - a_lp) * lp1;
        ph += syl_f / FS;
        if (ph >= 1.0f) {
            ph -= 1.0f;
        }
        float env = 0.25f + 0.75f * fabsf(sinf(PI_F * ph));
        float v = (on && i >= start) ? gain * env * lp2 * 4.0f : 0.0f;
        if (v > 32767.0f) {
            v = 32767.0f;
        } else if (v < -32768.0f) {
            v = -32768.0f;
        }
        x[i] = (int16_t)lrintf(v);
    }
}

/* Enclosure/room: 2 ms acoustic direct path with a small speaker ringing,
 * then a reverberant tail decaying with a 6 ms time constant (-72 dB at the
 * end of IR_LEN). */
static void gen_ir(float *h, uint32_t seed)
{
    uint32_t s = seed;
    memset(h, 0, IR_LEN * sizeof(float));
    h[32] = 0.5f;
    h[33] = -0.25f;
    h[34] = 0.12f;
    h[35] = -0.05f;
    for (int k = 36; k < IR_LEN; k++) {
        h[k] = 0.25f * expf(-(float)(k - 36) / 96.0f) * rnd(&s);
    }
}

/* Small loudspeaker: soft saturation (tanh), ~ -30 dB of distortion at the
 * peaks of a -20 dBFS talker. The canceller can only remove the linear part. */
static float speaker(int16_t x)
{
    return 32768.0f / 1.5f * tanhf(1.5f * (float)x / 32768.0f);
}

/* mic[n] = (h * speaker(far))[n - delay] + near[n] + noise (rms ~20 LSB,
 * ~ -64 dBFS: a quiet room through a MEMS mic) */
#define MIC_NOISE   35.0f               /* uniform [-35, 35): rms 20 */

static void gen_mic(const int16_t *far, const int16_t *near, const float *h, int delay, int16_t *mic, size_t n)
{
    uint32_t s = 12345;
    float *played = malloc(n * sizeof(float));
    for (size_t i = 0; i < n; i++) {
        played[i] = speaker(far[i]);
    }
    for (size_t i = 0; i < n; i++) {
        float e = 0;
        for (int k = 0; k < IR_LEN; k++) {
            long j = (long)i - delay - k;
            if (j >= 0) {
                e += h[k] * played[j];
            }
        }
        float v = e + (near ? near[i] : 0) + MIC_NOISE * rnd(&s);
        if (v > 32767.0f) {
            v = 32767.0f;
        } else if (v < -32768.0f) {
            v = -32768.0f;
        }
        mic[i] = (int16_t)lrintf(v);
    }
    free(played);
}

/* SpeexDSP's input DC notch (mdf.c filter_dc_notch16, float build, radius
 * 0.982 at 16 kHz): the canceller's output with no echo is this, not the
 * raw input. */
static void dc_notch(const int16_t *in, float *out, size_t n)
{
    const float r = 0.982f, den2 = r * r + 0.7f * (1.0f - r) * (1.0f - r);
    float m0 = 0, m1 = 0;
    for (size_t i = 0; i < n; i++) {
        float vin = in[i];
        float vout = m0 + vin;
        m0 = m1 + 2.0f * (-vin + r * vout);
        m1 = vin - den2 * vout;
        out[i] = r * vout;
    }
}

/* The device's reference path: the TX side writes what it plays into the
 * ring by sample index, the RX side reads index - aec_ref_offset(DELAY). */
static void run_aec(aec_t *a, const int16_t *far, const int16_t *mic, int16_t *out, size_t n)
{
    static int16_t ring_buf[RING];
    aec_ring_t ring;
    int16_t ref[FRAME];
    aec_ring_init(&ring, ring_buf, RING);
    for (size_t pos = 0; pos + FRAME <= n; pos += FRAME) {
        aec_ring_write(&ring, (uint32_t)pos, far + pos, FRAME);
        size_t late = aec_ring_read(&ring, (uint32_t)pos - aec_ref_offset(DELAY), (uint32_t)(pos + FRAME), ref,
                                    FRAME);
        TEST_ASSERT_EQUAL_size_t(0, late);
        aec_process(a, mic + pos, ref, out + pos);
    }
}

static double energy(const int16_t *x, size_t from, size_t to)
{
    double acc = 0;
    for (size_t i = from; i < to; i++) {
        acc += (double)x[i] * x[i];
    }
    return acc;
}

static double db(double num, double den)
{
    return 10.0 * log10(num / den);
}

/* in vs out over [from, to), out shifted by the canceller's latency */
static double erle_db(const aec_t *a, const int16_t *mic, const int16_t *out, size_t from, size_t to)
{
    uint32_t lat = aec_latency(a);
    return db(energy(mic, from, to), energy(out + lat, from, to));
}

static aec_t *make(uint32_t tail, bool suppress)
{
    aec_config_t c;
    aec_config_default(&c);
    c.tail_samples = tail;
    if (!suppress) {
        c.echo_suppress_db = c.echo_suppress_active_db = c.noise_suppress_db = 0;
    }
    aec_t *a = NULL;
    TEST_ASSERT_EQUAL_INT(AEC_OK, aec_create(&c, &a));
    TEST_ASSERT_NOT_NULL(a);
    return a;
}

/* ------------------------------------------------------------------ */
/* synthetic echo                                                     */
/* ------------------------------------------------------------------ */

#define ECHO_SECONDS    12
#define CONVERGE_S      4               /* ERLE measured from here to the end */

/* echo-only mic (echo at about the reference level, ERL ~0 dB); returns
 * ERLE after CONVERGE_S and fills ERLE per second */
static double echo_case(int misalign, bool suppress, double *erle_1s, size_t n_1s)
{
    const size_t n = ECHO_SECONDS * FS;
    int16_t *far = malloc(n * 2), *mic = malloc(n * 2), *out = malloc((n + FRAME) * 2);
    float h[IR_LEN];
    gen_speech(far, n, 1, 3000.0f, 0);
    gen_ir(h, 7);
    gen_mic(far, NULL, h, DELAY + misalign, mic, n);
    memset(out, 0, (n + FRAME) * 2);

    aec_t *a = make(4 * FRAME, suppress);
    uint32_t calls = aec_alloc_calls();
    run_aec(a, far, mic, out, n);
    TEST_ASSERT_EQUAL_UINT32(calls, aec_alloc_calls());         /* nothing allocated while processing */

    for (size_t s = 0; s < n_1s && s < ECHO_SECONDS; s++) {
        erle_1s[s] = erle_db(a, mic, out, s * FS, (s + 1) * FS - FRAME);
    }
    double erle = erle_db(a, mic, out, CONVERGE_S * FS, n - FRAME);
    aec_stats_t st;
    aec_get_stats(a, &st);
    TEST_ASSERT_EQUAL_UINT32(0, st.resets);
    TEST_ASSERT_TRUE(st.ref_frames > 0);
    printf("echo misalign %+d, %s: ERLE %.1f dB after %d s; stats erl %.1f erle %.1f recent %.1f; per s:",
           misalign, suppress ? "linear+suppressor" : "linear only", erle, CONVERGE_S,
           (double)aec_ratio_db(st.e_ref, st.e_mic), (double)aec_ratio_db(st.e_mic, st.e_out),
           (double)st.erle_recent_db);
    for (size_t s = 0; s < n_1s && s < ECHO_SECONDS; s++) {
        printf(" %.0f", erle_1s[s]);
    }
    printf("\n");
    aec_destroy(a);
    free(far);
    free(mic);
    free(out);
    return erle;
}

/* Thresholds: the achieved value minus ~5 dB (see the printed numbers;
 * linear only ~25 dB, limited by the speaker's distortion; with the
 * suppressor ~39 dB). Convergence: ERLE over the second second (1-2 s). */
#define ERLE_LINEAR_MIN     20.0
#define ERLE_SUPPRESS_MIN   34.0
#define ERLE_2S_MIN         15.0

static void test_echo_linear(void)
{
    double s1[ECHO_SECONDS];
    for (int m = -40; m <= 40; m += 40) {
        double e = echo_case(m, false, s1, ECHO_SECONDS);
        TEST_ASSERT_TRUE_MESSAGE(e >= ERLE_LINEAR_MIN, "linear ERLE");
        TEST_ASSERT_TRUE_MESSAGE(s1[1] >= ERLE_2S_MIN, "slow convergence");
    }
}

static void test_echo_with_suppressor(void)
{
    double s1[ECHO_SECONDS];
    for (int m = -40; m <= 40; m += 40) {
        double e = echo_case(m, true, s1, ECHO_SECONDS);
        TEST_ASSERT_TRUE_MESSAGE(e >= ERLE_SUPPRESS_MIN, "ERLE with suppressor");
        TEST_ASSERT_TRUE_MESSAGE(s1[1] >= ERLE_2S_MIN, "slow convergence");
    }
}

/* ------------------------------------------------------------------ */
/* double talk                                                        */
/* ------------------------------------------------------------------ */

/* 0-5 s far end alone (converge), 5-10 s both talk, 10-14 s far end alone.
 * Near end ~10 dB below the echo at the mic (loud speaker, quiet talker: the
 * hard case for double-talk robustness).
 * Criteria:
 *  - near end preserved: projection gain of the output onto the clean near
 *    signal, <out, near> / <near, near>, over 5-10 s >= NEAR_GAIN_MIN
 *    (linear only: ~1; the suppressor is allowed its -15 dB "active" limit
 *    in bins where echo dominates, but most near-end energy must pass)
 *  - no divergence: ERLE over 11-14 s is at least the converged value of
 *    the far-end-only test minus a few dB, and SpeexDSP never reset. */
#define DT_SECONDS      14
#define NEAR_GAIN_LINEAR_MIN    0.9
#define NEAR_GAIN_SUPPRESS_MIN  0.7

static void dt_case(bool suppress)
{
    const size_t n = DT_SECONDS * FS;
    int16_t *far = malloc(n * 2), *near = malloc(n * 2), *mic = malloc(n * 2), *out = malloc((n + FRAME) * 2);
    float h[IR_LEN];
    gen_speech(far, n, 1, 3000.0f, 0);
    gen_speech(near, n, 99, 1000.0f, 5 * FS);
    for (size_t i = 10 * FS; i < n; i++) {
        near[i] = 0;
    }
    gen_ir(h, 7);
    gen_mic(far, near, h, DELAY, mic, n);
    memset(out, 0, (n + FRAME) * 2);

    aec_t *a = make(4 * FRAME, suppress);
    run_aec(a, far, mic, out, n);
    uint32_t lat = aec_latency(a);

    double dot = 0, nn = 0;
    for (size_t i = 5 * FS; i < 10 * FS; i++) {
        dot += (double)out[i + lat] * near[i];
        nn += (double)near[i] * near[i];
    }
    double g = dot / nn;
    double after = erle_db(a, mic, out, 11 * FS, n - FRAME);
    double echo_in_dt = db(energy(mic, 5 * FS, 10 * FS) - nn, nn);
    aec_stats_t st;
    aec_get_stats(a, &st);
    printf("double talk, %s: echo/near %.1f dB, near-end gain %.2f (%.1f dB), ERLE after %.1f dB, resets %u\n",
           suppress ? "linear+suppressor" : "linear only", echo_in_dt, g, 20.0 * log10(g), after,
           (unsigned)st.resets);
    TEST_ASSERT_TRUE_MESSAGE(g >= (suppress ? NEAR_GAIN_SUPPRESS_MIN : NEAR_GAIN_LINEAR_MIN), "near end preserved");
    TEST_ASSERT_TRUE_MESSAGE(after >= (suppress ? ERLE_SUPPRESS_MIN : ERLE_LINEAR_MIN) - 5.0, "diverged");
    TEST_ASSERT_EQUAL_UINT32(0, st.resets);
    aec_destroy(a);
    free(far);
    free(near);
    free(mic);
    free(out);
}

static void test_double_talk_linear(void)
{
    dt_case(false);
}

static void test_double_talk_with_suppressor(void)
{
    dt_case(true);
}

/* ------------------------------------------------------------------ */
/* far-end silence                                                    */
/* ------------------------------------------------------------------ */

/* No far-end signal at all: the output must be the input (one frame later
 * with the suppressor), at normal and at low speech level. The reference is
 * the input through SpeexDSP's DC notch (a deliberate ~20 Hz high-pass whose
 * phase shift alone would make a raw comparison fail). Criteria: the
 * difference is >= SILENCE_SNR_MIN dB below the input, and the level is
 * within SILENCE_LEVEL_DB. */
#define SILENCE_SNR_MIN     25.0
#define SILENCE_LEVEL_DB    0.5

static void silence_case(float rms, bool suppress)
{
    const size_t n = 6 * FS;
    int16_t *far = calloc(n, 2), *near = malloc(n * 2), *mic = malloc(n * 2), *out = malloc((n + FRAME) * 2);
    float h[IR_LEN];
    gen_speech(near, n, 5, rms, 0);
    gen_ir(h, 7);
    gen_mic(far, near, h, DELAY, mic, n);
    memset(out, 0, (n + FRAME) * 2);

    aec_t *a = make(4 * FRAME, suppress);
    run_aec(a, far, mic, out, n);
    uint32_t lat = aec_latency(a);
    float *want = malloc(n * sizeof(float));
    dc_notch(mic, want, n);
    double err = 0, in = 0;
    for (size_t i = FS; i < n - FRAME; i++) {
        double d = (double)out[i + lat] - (double)want[i];
        err += d * d;
        in += (double)want[i] * (double)want[i];
    }
    double snr = db(in, err);
    double level = db(energy(out + lat, FS, n - FRAME), in);
    printf("far-end silence, near rms %.0f, %s: output/input %.2f dB, difference %.1f dB below input\n",
           (double)rms, suppress ? "linear+suppressor" : "linear only", level, snr);
    TEST_ASSERT_TRUE_MESSAGE(snr >= SILENCE_SNR_MIN, "output differs from input");
    TEST_ASSERT_TRUE_MESSAGE(fabs(level) <= SILENCE_LEVEL_DB, "output level changed");
    free(want);
    aec_destroy(a);
    free(far);
    free(near);
    free(mic);
    free(out);
}

static void test_far_end_silence(void)
{
    silence_case(1000.0f, false);
    silence_case(1000.0f, true);
    silence_case(60.0f, true);                  /* quiet talker, ~ -55 dBFS */
}

/* ------------------------------------------------------------------ */
/* determinism, allocation, settings                                  */
/* ------------------------------------------------------------------ */

static void test_deterministic(void)
{
    const size_t n = 3 * FS;
    int16_t *far = malloc(n * 2), *near = malloc(n * 2), *mic = malloc(n * 2);
    int16_t *o1 = malloc(n * 2), *o2 = malloc(n * 2);
    float h[IR_LEN];
    gen_speech(far, n, 1, 3000.0f, 0);
    gen_speech(near, n, 2, 800.0f, FS);
    gen_ir(h, 7);
    gen_mic(far, near, h, DELAY, mic, n);

    aec_t *a = make(4 * FRAME, true);
    aec_t *b = make(4 * FRAME, true);
    run_aec(a, far, mic, o1, n);
    run_aec(b, far, mic, o2, n);
    TEST_ASSERT_EQUAL_MEMORY(o1, o2, n * 2);
    /* in place (out aliases mic) gives the same result */
    aec_t *c = make(4 * FRAME, true);
    int16_t *inplace = malloc(n * 2);
    memcpy(inplace, mic, n * 2);
    run_aec(c, far, inplace, inplace, n);
    TEST_ASSERT_EQUAL_MEMORY(o1, inplace, n * 2);
    aec_destroy(a);
    aec_destroy(b);
    aec_destroy(c);
    free(far);
    free(near);
    free(mic);
    free(o1);
    free(o2);
    free(inplace);
}

static uint32_t s_live, s_calls;
static size_t s_bytes;

static void *count_alloc(size_t size)
{
    s_live++;
    s_calls++;
    s_bytes += size;
    return calloc(1, size);
}

static void count_free(void *p)
{
    s_live--;
    free(p);
}

static void test_allocator_hook(void)
{
    aec_set_allocator(count_alloc, count_free);
    s_live = s_calls = 0;
    s_bytes = 0;
    for (uint32_t tail = FRAME; tail <= 8 * FRAME; tail += FRAME) {
        aec_t *a = make(tail, true);
        size_t bytes = s_bytes;
        TEST_ASSERT_EQUAL_size_t(bytes, aec_mem_bytes(a));
        TEST_ASSERT_TRUE(bytes <= aec_mem_estimate(tail));
        TEST_ASSERT_TRUE(aec_mem_estimate(tail) - bytes <= 1024);
        int16_t z[FRAME] = {0};
        uint32_t calls = s_calls;
        for (int i = 0; i < 50; i++) {
            aec_process(a, z, z, z);
        }
        TEST_ASSERT_EQUAL_UINT32(calls, s_calls);
        aec_destroy(a);
        TEST_ASSERT_EQUAL_UINT32(0, s_live);
        printf("tail %4u: %zu bytes in %u allocations\n", (unsigned)tail, bytes, (unsigned)calls);
        s_bytes = 0;
        s_calls = 0;
    }
    aec_set_allocator(NULL, NULL);
}

static void test_config_and_suppress(void)
{
    aec_config_t c;
    aec_t *a = NULL;
    aec_config_default(&c);
    c.tail_samples = 1000;
    TEST_ASSERT_EQUAL_INT(AEC_OK, aec_create(&c, &a));
    aec_config_t got;
    aec_get_config(a, &got);
    TEST_ASSERT_EQUAL_UINT32(1280, got.tail_samples);           /* whole frames */
    TEST_ASSERT_EQUAL_INT(AEC_ECHO_SUPPRESS_DEFAULT, got.echo_suppress_db);
    TEST_ASSERT_EQUAL_UINT32(FRAME, aec_latency(a));
    TEST_ASSERT_EQUAL_INT(AEC_ERR_ARG, aec_set_suppress(a, 5, -15, 0));
    TEST_ASSERT_EQUAL_INT(AEC_ERR_ARG, aec_set_suppress(a, -40, -100, 0));
    TEST_ASSERT_EQUAL_INT(AEC_OK, aec_set_suppress(a, 0, 0, 0));
    TEST_ASSERT_EQUAL_UINT32(0, aec_latency(a));
    TEST_ASSERT_EQUAL_INT(AEC_OK, aec_set_suppress(a, -30, -10, -6));
    aec_get_config(a, &got);
    TEST_ASSERT_EQUAL_INT(-30, got.echo_suppress_db);
    TEST_ASSERT_EQUAL_INT(-10, got.echo_suppress_active_db);
    TEST_ASSERT_EQUAL_INT(-6, got.noise_suppress_db);
    TEST_ASSERT_EQUAL_UINT32(FRAME, aec_latency(a));
    aec_destroy(a);

    c.tail_samples = 0;
    TEST_ASSERT_EQUAL_INT(AEC_ERR_ARG, aec_create(&c, &a));
    c.tail_samples = AEC_TAIL_MAX + 1;
    TEST_ASSERT_EQUAL_INT(AEC_ERR_ARG, aec_create(&c, &a));
    TEST_ASSERT_EQUAL_UINT32(0, aec_ref_offset(100));
    TEST_ASSERT_EQUAL_UINT32(DELAY - AEC_REF_LEAD, aec_ref_offset(DELAY));
}

/* ------------------------------------------------------------------ */
/* reference ring + helpers (moved from the audio component)          */
/* ------------------------------------------------------------------ */

static void test_ring_read_by_index(void)
{
    static int16_t buf[1024];
    aec_ring_t r;
    aec_ring_init(&r, buf, 1024);
    int16_t chunk[160];
    uint32_t written = 0;
    for (int f = 0; f < 10; f++) {                  /* 1600 samples, value = index & 0x7fff */
        for (int i = 0; i < 160; i++) {
            chunk[i] = (int16_t)((written + (uint32_t)i) & 0x7fff);
        }
        aec_ring_write(&r, written, chunk, 160);
        written += 160;
    }
    int16_t out[16];
    TEST_ASSERT_EQUAL_size_t(0, aec_ring_read(&r, 1000, written, out, 16));
    for (int i = 0; i < 16; i++) {
        TEST_ASSERT_EQUAL_INT16(1000 + i, out[i]);
    }
    /* newest 8 stored, 8 in the future */
    TEST_ASSERT_EQUAL_size_t(8, aec_ring_read(&r, written - 8, written, out, 16));
    for (int i = 0; i < 16; i++) {
        TEST_ASSERT_EQUAL_INT16(i < 8 ? (int16_t)(written - 8 + (uint32_t)i) : 0, out[i]);
    }
    /* overwritten: older than the ring size */
    TEST_ASSERT_EQUAL_size_t(0, aec_ring_read(&r, written - 1024 - 8, written, out, 16));
    for (int i = 0; i < 16; i++) {
        TEST_ASSERT_EQUAL_INT16(i < 8 ? 0 : (int16_t)(written - 1024 + (uint32_t)(i - 8)), out[i]);
    }
}

static void test_ring_session_start_and_wrap(void)
{
    static int16_t buf[256];
    aec_ring_t r;
    aec_ring_init(&r, buf, 256);
    int16_t in[32], out[32];
    for (int i = 0; i < 32; i++) {
        in[i] = (int16_t)(i + 1);
    }
    /* session start: reader asks for "index -8" (0 - delay) */
    aec_ring_write(&r, 0, in, 32);
    TEST_ASSERT_EQUAL_size_t(0, aec_ring_read(&r, (uint32_t)-8, 32, out, 16));
    for (int i = 0; i < 16; i++) {
        TEST_ASSERT_EQUAL_INT16(i < 8 ? 0 : i - 8 + 1, out[i]);
    }
    /* 32-bit counter wrap between writer and reader */
    uint32_t pos = UINT32_MAX - 15;
    aec_ring_init(&r, buf, 256);
    aec_ring_write(&r, pos, in, 32);
    TEST_ASSERT_EQUAL_size_t(0, aec_ring_read(&r, pos, pos + 32, out, 32));
    TEST_ASSERT_EQUAL_INT16_ARRAY(in, out, 32);
}

static void test_energy_db(void)
{
    int16_t a[100], b[100];
    for (int i = 0; i < 100; i++) {
        a[i] = (int16_t)(i % 2 ? 1000 : -1000);
        b[i] = (int16_t)(i % 2 ? 100 : -100);
    }
    TEST_ASSERT_EQUAL_UINT64(100ull * 1000 * 1000, aec_energy(a, 100));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 20.0f, aec_ratio_db(aec_energy(a, 100), aec_energy(b, 100)));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, -20.0f, aec_ratio_db(aec_energy(b, 100), aec_energy(a, 100)));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, aec_ratio_db(0, 5));
    int16_t fs[512];
    for (int i = 0; i < 512; i++) {
        fs[i] = INT16_MIN;
    }
    TEST_ASSERT_EQUAL_UINT64(512ull * 32768 * 32768, aec_energy(fs, 512));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_ring_read_by_index);
    RUN_TEST(test_ring_session_start_and_wrap);
    RUN_TEST(test_energy_db);
    RUN_TEST(test_config_and_suppress);
    RUN_TEST(test_allocator_hook);
    RUN_TEST(test_deterministic);
    RUN_TEST(test_echo_linear);
    RUN_TEST(test_echo_with_suppressor);
    RUN_TEST(test_double_talk_linear);
    RUN_TEST(test_double_talk_with_suppressor);
    RUN_TEST(test_far_end_silence);
    return UNITY_END();
}
