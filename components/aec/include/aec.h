#pragma once

/* Acoustic echo canceller for the 16 kHz mic path: SpeexDSP's MDF adaptive
 * filter (frequency-domain, partitioned, two-path) followed by SpeexDSP's
 * residual echo suppressor (speex_preprocess with the echo state attached),
 * vendored in speexdsp/ (BSD-3, see speexdsp/README.md). Pure C, no IDF
 * headers: host-buildable and host-tested like codec/rtp/sip.
 *
 * Works on whole 20 ms frames (AEC_FRAME_SAMPLES): one speex frame per call,
 * no block adaptation. The linear filter adds no latency; the suppressor's
 * overlap-add adds exactly one frame (AEC_FRAME_SAMPLES) while it is on.
 *
 * Also here, because they are pure and testable: the reference delay line the
 * audio tasks share (aec_ring_*) and the energy helpers behind ERL/ERLE.
 *
 * An aec_t is owned by one thread; nothing here locks. All memory is taken in
 * aec_create() through the allocator hook (aec_set_allocator): processing
 * never allocates. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AEC_SAMPLE_RATE         16000
#define AEC_FRAME_SAMPLES       320     /* 20 ms */
#define AEC_TAIL_MIN            AEC_FRAME_SAMPLES
#define AEC_TAIL_MAX            (16 * AEC_FRAME_SAMPLES)        /* 320 ms */

/* Residual suppression defaults: SpeexDSP's own (preprocess.c). */
#define AEC_ECHO_SUPPRESS_DEFAULT           (-40)
#define AEC_ECHO_SUPPRESS_ACTIVE_DEFAULT    (-15)
#define AEC_SUPPRESS_MIN_DB                 (-90)

typedef enum {
    AEC_OK = 0,
    AEC_ERR_ARG = -1,
    AEC_ERR_NO_MEM = -2,
} aec_err_t;

typedef struct {
    /* Echo path length the adaptive filter models, in samples; rounded up to
     * whole frames (the filter has ceil(tail / 320) partitions). */
    uint32_t tail_samples;
    /* Residual echo suppressor, attenuation limits in dB (<= 0):
     *   echo_suppress_db         far end talking alone (SpeexDSP ECHO_SUPPRESS)
     *   echo_suppress_active_db  near end talking too (ECHO_SUPPRESS_ACTIVE)
     *   noise_suppress_db        stationary noise (NOISE_SUPPRESS), 0 = off
     * All three 0 = suppressor bypassed: linear filter only, no latency. */
    int echo_suppress_db;
    int echo_suppress_active_db;
    int noise_suppress_db;
} aec_config_t;

typedef struct aec aec_t;

/** tail 1280, echo -40 / active -15 dB, noise off. */
void aec_config_default(aec_config_t *cfg);

/** Allocate and initialise a canceller (all its memory, through the hook). */
aec_err_t aec_create(const aec_config_t *cfg, aec_t **out);
void aec_destroy(aec_t *a);
/** Forget the learned echo path (filter and suppressor history). */
void aec_reset(aec_t *a);

/** One frame: mic = captured (echo + near end), ref = what the speaker played
 *  for the same frame, aligned so the echo follows it within the tail; out
 *  may alias mic (not ref). */
void aec_process(aec_t *a, const int16_t mic[AEC_FRAME_SAMPLES], const int16_t ref[AEC_FRAME_SAMPLES],
                 int16_t out[AEC_FRAME_SAMPLES]);

/** Change the residual suppression levels (see aec_config_t); takes effect
 *  on the next frame. Switching the suppressor on or off steps the latency. */
aec_err_t aec_set_suppress(aec_t *a, int echo_db, int echo_active_db, int noise_db);
void aec_get_config(const aec_t *a, aec_config_t *cfg);
/** Samples the output lags the input by (0 or AEC_FRAME_SAMPLES). */
uint32_t aec_latency(const aec_t *a);
/** Bytes this instance took from the allocator. */
size_t aec_mem_bytes(const aec_t *a);
/** Upper bound of aec_mem_bytes() for a tail (exact on 64-bit hosts, a few
 *  hundred bytes over on 32-bit targets; heap block overhead not included).
 *  SpeexDSP does not check its allocations, so the caller checks this much
 *  memory is there before aec_create(). */
size_t aec_mem_estimate(uint32_t tail_samples);

/* ---- statistics (since create or aec_clear_stats) ---- */

typedef struct {
    uint32_t frames;            /* frames processed */
    uint32_t ref_frames;        /* frames with far-end signal (counted for ERL/ERLE) */
    uint32_t resets;            /* filter divergence resets done by SpeexDSP itself */
    /* energies over the ref_frames, mic and out latency-aligned */
    uint64_t e_ref, e_mic, e_out;
    float erle_recent_db;       /* mic -> out over the last ~2 s with far-end signal, 0 = none yet */
} aec_stats_t;

void aec_get_stats(const aec_t *a, aec_stats_t *st);
void aec_clear_stats(aec_t *a);

/* ---- allocator hook ---- */

/** Must return zeroed memory (calloc-like) or NULL. */
typedef void *(*aec_alloc_fn_t)(size_t size);
typedef void (*aec_free_fn_t)(void *ptr);

/** Allocator for everything aec_create() takes (process-wide; set it before
 *  the first create). NULL, NULL = calloc / free. */
void aec_set_allocator(aec_alloc_fn_t alloc_fn, aec_free_fn_t free_fn);
/** Calls made to the allocator so far, process-wide (tests: none after create). */
uint32_t aec_alloc_calls(void);

/* ---- reference delay line ---- */

/* Speaker samples stored by absolute sample index (the TX side's running
 * count), read back by index on the mic side. No locking here; the caller
 * publishes `written` after aec_ring_write. Re-init (zero) it when the index
 * restarts at 0. */
typedef struct {
    int16_t *buf;
    uint32_t size;              /* power of two */
} aec_ring_t;

void aec_ring_init(aec_ring_t *r, int16_t *buf, uint32_t size_pow2);
/** Store in[0..n) at indices pos..pos+n-1 (mod size). */
void aec_ring_write(aec_ring_t *r, uint32_t pos, const int16_t *in, size_t n);
/** Copy indices pos..pos+n-1 to out, given that indices 0..written-1 were
 *  stored since init (modulo 2^32, so an index "before 0", e.g. 0 - delay at
 *  the start of a session, is fine). Not yet written, overwritten and pre-start
 *  indices read as 0. Returns how many samples were not written yet (the
 *  reader is ahead of the writer). */
size_t aec_ring_read(const aec_ring_t *r, uint32_t pos, uint32_t written, int16_t *out, size_t n);

/** Reference read offset for a nominal speaker->mic delay: the reference is
 *  read AEC_REF_LEAD samples early, so an echo that arrives up to that much
 *  before the nominal delay still falls inside the filter (it cannot model a
 *  non-causal path). delay < lead -> 0. */
#define AEC_REF_LEAD            160     /* 10 ms: one I2S DMA buffer */
uint32_t aec_ref_offset(uint32_t delay);

/* ---- helpers ---- */

/** Sum of squares (energy) of n samples. */
uint64_t aec_energy(const int16_t *x, size_t n);
/** 10*log10(num/den) in dB; 0 when either is 0. */
float aec_ratio_db(uint64_t num, uint64_t den);

#ifdef __cplusplus
}
#endif
