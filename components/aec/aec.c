#include "aec.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "speex/speex_echo.h"
#include "speex/speex_preprocess.h"

/* A frame counts for ERL/ERLE when the reference is above ~-50 dBFS RMS. */
#define REF_ACTIVE_MEANSQ       (100 * 100)
#define RECENT_SAMPLES          (2 * AEC_SAMPLE_RATE)   /* erle_recent window: 2 s of far-end signal */

struct aec {
    SpeexEchoState *echo;
    SpeexPreprocessState *pre;
    aec_config_t cfg;
    bool suppress;                      /* preprocess in the path */
    size_t mem_bytes;
    int16_t lin[AEC_FRAME_SAMPLES];     /* linear filter output (speex needs out != in) */

    /* statistics; with the suppressor on, out lags mic by one frame, so mic
     * energy and ref activity are held one frame to compare like with like */
    aec_stats_t st;
    uint64_t prev_e_mic, prev_e_ref;
    uint64_t w_mic, w_out;
    uint32_t w_samples;
};

/* ------------------------------------------------------------------ */
/* allocator hook + SpeexDSP OS support (os_support_custom.h)         */
/* ------------------------------------------------------------------ */

static void *default_alloc(size_t size)
{
    return calloc(1, size);
}

static aec_alloc_fn_t s_alloc = default_alloc;
static aec_free_fn_t s_free = free;
static uint32_t s_alloc_calls;
static size_t s_alloc_bytes;            /* cumulative, for aec_mem_bytes */
static uint32_t s_speex_resets;         /* "acting funny" resets, all instances */

void aec_set_allocator(aec_alloc_fn_t alloc_fn, aec_free_fn_t free_fn)
{
    s_alloc = alloc_fn ? alloc_fn : default_alloc;
    s_free = free_fn ? free_fn : free;
}

uint32_t aec_alloc_calls(void)
{
    return s_alloc_calls;
}

static void *mem_alloc(size_t size)
{
    s_alloc_calls++;
    s_alloc_bytes += size;
    return s_alloc(size);
}

void *aec_speex_alloc(int size)
{
    /* SpeexDSP does not check for NULL: aec_create() checks every pointer it
     * can see, and runs on an allocator that fails rarely (init only). */
    return size > 0 ? mem_alloc((size_t)size) : NULL;
}

void aec_speex_free(void *ptr)
{
    if (ptr) {
        s_free(ptr);
    }
}

void aec_speex_warning(const char *str)
{
    /* the only warning the processing path can raise (mdf.c): the filter
     * diverged and SpeexDSP reset it */
    if (strncmp(str, "The echo canceller started acting funny", 39) == 0) {
        s_speex_resets++;
    }
}

void aec_speex_fatal(const char *str)
{
    (void)str;
    abort();
}

/* ------------------------------------------------------------------ */
/* canceller                                                          */
/* ------------------------------------------------------------------ */

void aec_config_default(aec_config_t *cfg)
{
    cfg->tail_samples = 4 * AEC_FRAME_SAMPLES;
    cfg->echo_suppress_db = AEC_ECHO_SUPPRESS_DEFAULT;
    cfg->echo_suppress_active_db = AEC_ECHO_SUPPRESS_ACTIVE_DEFAULT;
    cfg->noise_suppress_db = 0;
}

static bool db_ok(int db)
{
    return db <= 0 && db >= AEC_SUPPRESS_MIN_DB;
}

static void apply_suppress(aec_t *a)
{
    spx_int32_t v = a->cfg.echo_suppress_db;
    speex_preprocess_ctl(a->pre, SPEEX_PREPROCESS_SET_ECHO_SUPPRESS, &v);
    v = a->cfg.echo_suppress_active_db;
    speex_preprocess_ctl(a->pre, SPEEX_PREPROCESS_SET_ECHO_SUPPRESS_ACTIVE, &v);
    v = a->cfg.noise_suppress_db;
    speex_preprocess_ctl(a->pre, SPEEX_PREPROCESS_SET_NOISE_SUPPRESS, &v);
    a->suppress = a->cfg.echo_suppress_db != 0 || a->cfg.echo_suppress_active_db != 0 ||
                  a->cfg.noise_suppress_db != 0;
}

aec_err_t aec_create(const aec_config_t *cfg, aec_t **out)
{
    if (cfg == NULL || out == NULL || cfg->tail_samples < AEC_TAIL_MIN || cfg->tail_samples > AEC_TAIL_MAX ||
        !db_ok(cfg->echo_suppress_db) || !db_ok(cfg->echo_suppress_active_db) || !db_ok(cfg->noise_suppress_db)) {
        return AEC_ERR_ARG;
    }
    *out = NULL;
    const size_t bytes0 = s_alloc_bytes;
    aec_t *a = mem_alloc(sizeof(*a));
    if (a == NULL) {
        return AEC_ERR_NO_MEM;
    }
    a->cfg = *cfg;
    a->cfg.tail_samples = (cfg->tail_samples + AEC_FRAME_SAMPLES - 1) / AEC_FRAME_SAMPLES * AEC_FRAME_SAMPLES;

    /* SpeexDSP's init functions dereference their own allocations without
     * checking: a failure in there crashes rather than returns, so the
     * caller makes sure the memory is there (aec_mem_bytes of a test
     * instance; ~45 KB at the default tail). */
    a->echo = speex_echo_state_init((int)AEC_FRAME_SAMPLES, (int)a->cfg.tail_samples);
    a->pre = a->echo ? speex_preprocess_state_init((int)AEC_FRAME_SAMPLES, AEC_SAMPLE_RATE) : NULL;
    if (a->echo == NULL || a->pre == NULL) {
        aec_destroy(a);
        return AEC_ERR_NO_MEM;
    }
    spx_int32_t rate = AEC_SAMPLE_RATE;
    speex_echo_ctl(a->echo, SPEEX_ECHO_SET_SAMPLING_RATE, &rate);
    speex_preprocess_ctl(a->pre, SPEEX_PREPROCESS_SET_ECHO_STATE, a->echo);
    /* the suppressor only applies its gains with "denoise" on; noise
     * suppression itself is noise_suppress_db (0 dB = none) */
    spx_int32_t on = 1, off = 0;
    speex_preprocess_ctl(a->pre, SPEEX_PREPROCESS_SET_DENOISE, &on);
    speex_preprocess_ctl(a->pre, SPEEX_PREPROCESS_SET_AGC, &off);
    speex_preprocess_ctl(a->pre, SPEEX_PREPROCESS_SET_VAD, &off);
    speex_preprocess_ctl(a->pre, SPEEX_PREPROCESS_SET_DEREVERB, &off);
    apply_suppress(a);

    a->mem_bytes = s_alloc_bytes - bytes0;
    *out = a;
    return AEC_OK;
}

void aec_destroy(aec_t *a)
{
    if (a == NULL) {
        return;
    }
    if (a->pre) {
        speex_preprocess_state_destroy(a->pre);
    }
    if (a->echo) {
        speex_echo_state_destroy(a->echo);
    }
    s_free(a);
}

void aec_reset(aec_t *a)
{
    speex_echo_state_reset(a->echo);
    aec_clear_stats(a);
}

static void update_stats(aec_t *a, const int16_t *mic, const int16_t *ref, const int16_t *out)
{
    uint64_t e_mic = aec_energy(mic, AEC_FRAME_SAMPLES);
    uint64_t e_ref = aec_energy(ref, AEC_FRAME_SAMPLES);
    if (a->suppress) {
        uint64_t m = e_mic, r = e_ref;
        e_mic = a->prev_e_mic;
        e_ref = a->prev_e_ref;
        a->prev_e_mic = m;
        a->prev_e_ref = r;
    }
    a->st.frames++;
    if (e_ref < (uint64_t)REF_ACTIVE_MEANSQ * AEC_FRAME_SAMPLES) {
        return;                         /* no far-end signal: nothing to measure */
    }
    uint64_t e_out = aec_energy(out, AEC_FRAME_SAMPLES);
    a->st.ref_frames++;
    a->st.e_ref += e_ref;
    a->st.e_mic += e_mic;
    a->st.e_out += e_out;
    a->w_mic += e_mic;
    a->w_out += e_out;
    a->w_samples += AEC_FRAME_SAMPLES;
    if (a->w_samples >= RECENT_SAMPLES) {
        a->st.erle_recent_db = aec_ratio_db(a->w_mic, a->w_out);
        a->w_mic = a->w_out = 0;
        a->w_samples = 0;
    }
}

void aec_process(aec_t *a, const int16_t mic[AEC_FRAME_SAMPLES], const int16_t ref[AEC_FRAME_SAMPLES],
                 int16_t out[AEC_FRAME_SAMPLES])
{
    uint32_t resets0 = s_speex_resets;
    speex_echo_cancellation(a->echo, mic, ref, a->lin);
    if (s_speex_resets != resets0) {
        a->st.resets++;
    }
    if (a->suppress) {
        speex_preprocess_run(a->pre, a->lin);
    }
    update_stats(a, mic, ref, a->lin);  /* before out: it may alias mic */
    memcpy(out, a->lin, sizeof(a->lin));
}

aec_err_t aec_set_suppress(aec_t *a, int echo_db, int echo_active_db, int noise_db)
{
    if (!db_ok(echo_db) || !db_ok(echo_active_db) || !db_ok(noise_db)) {
        return AEC_ERR_ARG;
    }
    a->cfg.echo_suppress_db = echo_db;
    a->cfg.echo_suppress_active_db = echo_active_db;
    a->cfg.noise_suppress_db = noise_db;
    bool was = a->suppress;
    apply_suppress(a);
    if (a->suppress != was) {
        a->prev_e_mic = a->prev_e_ref = 0;
    }
    return AEC_OK;
}

void aec_get_config(const aec_t *a, aec_config_t *cfg)
{
    *cfg = a->cfg;
}

uint32_t aec_latency(const aec_t *a)
{
    return a->suppress ? AEC_FRAME_SAMPLES : 0;
}

size_t aec_mem_bytes(const aec_t *a)
{
    return a->mem_bytes;
}

/* Measured on the 64-bit host (test_allocator_hook checks it): MDF scratch,
 * FFT tables and the preprocessor are fixed for 320-sample frames; each
 * filter partition adds its weights, foreground weights and far-end spectrum
 * (3 x 640 floats) + 1 float. */
#define MEM_FIXED               93304u
#define MEM_PER_PARTITION       7684u

size_t aec_mem_estimate(uint32_t tail_samples)
{
    uint32_t m = (tail_samples + AEC_FRAME_SAMPLES - 1) / AEC_FRAME_SAMPLES;
    return MEM_FIXED + (size_t)m * MEM_PER_PARTITION;
}

void aec_get_stats(const aec_t *a, aec_stats_t *st)
{
    *st = a->st;
}

void aec_clear_stats(aec_t *a)
{
    memset(&a->st, 0, sizeof(a->st));
    a->prev_e_mic = a->prev_e_ref = 0;
    a->w_mic = a->w_out = 0;
    a->w_samples = 0;
}

/* ------------------------------------------------------------------ */
/* reference delay line                                               */
/* ------------------------------------------------------------------ */

void aec_ring_init(aec_ring_t *r, int16_t *buf, uint32_t size_pow2)
{
    r->buf = buf;
    r->size = size_pow2;
    memset(buf, 0, size_pow2 * sizeof(int16_t));
}

void aec_ring_write(aec_ring_t *r, uint32_t pos, const int16_t *in, size_t n)
{
    const uint32_t mask = r->size - 1;
    for (size_t i = 0; i < n; i++) {
        r->buf[(pos + i) & mask] = in[i];
    }
}

size_t aec_ring_read(const aec_ring_t *r, uint32_t pos, uint32_t written, int16_t *out, size_t n)
{
    const uint32_t mask = r->size - 1;
    size_t late = 0;
    for (size_t i = 0; i < n; i++) {
        /* age 1 = newest stored sample; modular, so it survives the counter
         * wrapping. A pre-start index is either older than the ring or lands
         * on a slot still zero from aec_ring_init. */
        uint32_t p = pos + (uint32_t)i;
        int32_t age = (int32_t)(written - p);
        if (age < 1) {
            out[i] = 0;
            late++;
        } else if ((uint32_t)age > r->size) {
            out[i] = 0;
        } else {
            out[i] = r->buf[p & mask];
        }
    }
    return late;
}

uint32_t aec_ref_offset(uint32_t delay)
{
    return delay > AEC_REF_LEAD ? delay - AEC_REF_LEAD : 0;
}

/* ------------------------------------------------------------------ */
/* helpers                                                            */
/* ------------------------------------------------------------------ */

uint64_t aec_energy(const int16_t *x, size_t n)
{
    uint64_t acc = 0;
    for (size_t i = 0; i < n; i++) {
        acc += (uint64_t)((int32_t)x[i] * x[i]);
    }
    return acc;
}

float aec_ratio_db(uint64_t num, uint64_t den)
{
    if (num == 0 || den == 0) {
        return 0.0f;
    }
    return 10.0f * log10f((float)num / (float)den);
}
