#pragma once

/* Pure DSP helpers for the audio component: no IDF headers, host-testable.
 * Everything here is single-threaded state owned by one task. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---- DC blocker: y[n] = x[n] - x[n-1] + R*y[n-1] ---- */

typedef struct {
    int32_t r_q15;
    int32_t px;
    int32_t y8;                     /* previous output, Q8 */
} dsp_dcblock_t;

void dsp_dcblock_init(dsp_dcblock_t *s, uint32_t fs_hz, uint32_t fc_hz);
int16_t dsp_dcblock_step(dsp_dcblock_t *s, int32_t x);
/** Input is wider than 16 bit (shifted mic samples); output is saturated. */
void dsp_dcblock_process(dsp_dcblock_t *s, const int32_t *in, int16_t *out, size_t n);

/* ---- gain + soft clip ---- */

#define DSP_GAIN_UNITY 32768        /* Q15 */

/** Volume 0-100 -> Q15 gain, squared (perceptual) curve. 0 -> 0, 100 -> unity. */
int32_t dsp_gain_q15(uint8_t volume);
/** buf[i] *= gain, linearly ramped from g0 (before the buffer) to g1 (at its end). */
void dsp_gain_ramp(int32_t *buf, size_t n, int32_t g0, int32_t g1);
/** Linear below 0.75 FS, smooth knee above; never wraps, |out| <= 32766. */
int16_t dsp_softclip(int32_t x);
void dsp_softclip_buf(const int32_t *in, int16_t *out, size_t n);

/* ---- tone generator (dual sine, 32-bit phase accumulator, 256-entry table) ---- */

typedef enum {
    DSP_TONE_DIAL,
    DSP_TONE_RINGBACK,
    DSP_TONE_BUSY,
    DSP_TONE_RING,
    DSP_TONE_KEYCLICK,
} dsp_tone_kind_t;

typedef struct {
    uint16_t f1, f2;                /* Hz; f2 == 0: single tone */
    uint16_t on_ms;                 /* 0: continuous */
    uint16_t period_ms;             /* on + off; ignored when continuous or one-shot */
    uint16_t warble_ms;             /* > 0: alternate f1/f2 every warble_ms (single oscillator) */
    bool oneshot;                   /* play on_ms once, then go inactive */
} dsp_tone_def_t;

typedef struct {
    uint32_t rate;
    uint32_t ph1, ph2, inc1, inc2;
    uint32_t on_n, period_n, pos;   /* samples */
    uint32_t warble_n, warble_pos;
    bool warble_hi;
    bool oneshot, active, stopping, single;
    int32_t env, env_step;          /* Q15 attack/release envelope */
    int32_t amp;                    /* Q15 per component */
} dsp_tone_t;

void dsp_tone_init(dsp_tone_t *t, uint32_t rate);
void dsp_tone_start_def(dsp_tone_t *t, const dsp_tone_def_t *def);
void dsp_tone_start(dsp_tone_t *t, dsp_tone_kind_t kind);
/** 100 ms DTMF pair; false (and no change) for an invalid digit. */
bool dsp_tone_start_dtmf(dsp_tone_t *t, char digit);
/** Release with a short ramp, then go inactive. */
void dsp_tone_stop(dsp_tone_t *t);
bool dsp_tone_active(const dsp_tone_t *t);
/** Add n samples of tone to acc (untouched while inactive / in the off phase). */
void dsp_tone_add(dsp_tone_t *t, int32_t *acc, size_t n);

/** Row/column frequencies of a DTMF digit (0-9 A-D * #); lo/hi may be NULL. */
bool dsp_dtmf_freqs(char digit, unsigned *lo, unsigned *hi);

/* ---- echo-canceller plumbing (the canceller itself is esp-sr, see audio_aec.c) ---- */

/* Reference delay line: speaker samples stored by absolute sample index (the
 * TX task's running count), read back by index by the RX task. No locking here;
 * the caller publishes `written` after dsp_ring_write. Re-init (zero) it when
 * the index restarts at 0. */
typedef struct {
    int16_t *buf;
    uint32_t size;                  /* power of two */
} dsp_ring_t;

void dsp_ring_init(dsp_ring_t *r, int16_t *buf, uint32_t size_pow2);
/** Store in[0..n) at indices pos..pos+n-1 (mod size). */
void dsp_ring_write(dsp_ring_t *r, uint32_t pos, const int16_t *in, size_t n);
/** Copy indices pos..pos+n-1 to out, given that indices 0..written-1 were
 *  stored since init (modulo 2^32, so an index "before 0", e.g. 0 - delay at
 *  the start of a session, is fine). Not yet written, overwritten and pre-start
 *  indices read as 0. Returns how many samples were not written yet (the
 *  reader is ahead of the writer). */
size_t dsp_ring_read(const dsp_ring_t *r, uint32_t pos, uint32_t written, int16_t *out, size_t n);

/* Frame adapter: the audio path runs fixed `frame`-sample frames, the
 * canceller wants `block`-sample blocks. Input is queued until a block is
 * full; output is pre-filled with `latency` zeros so a whole frame is always
 * ready (latency = block - gcd(frame, block), the smallest that works). */
typedef void (*dsp_block_fn_t)(void *ctx, int16_t *mic, int16_t *ref, int16_t *out);

typedef struct {
    size_t frame, block, latency;
    int16_t *mic, *ref, *proc;      /* block samples each (caller-allocated, e.g. aligned) */
    int16_t *out;                   /* dsp_blockfifo_out_size() samples */
    size_t in_fill, out_fill;
} dsp_blockfifo_t;

size_t dsp_blockfifo_latency(size_t frame, size_t block);
size_t dsp_blockfifo_out_size(size_t frame, size_t block);
void dsp_blockfifo_init(dsp_blockfifo_t *f, size_t frame, size_t block,
                        int16_t *mic, int16_t *ref, int16_t *proc, int16_t *out);
/** Drop queued samples and re-prime the output with `latency` zeros. */
void dsp_blockfifo_reset(dsp_blockfifo_t *f);
/** Queue one frame of mic + ref, call fn for each full block (fn writes `block`
 *  samples to its out), then pop one frame into out (may alias mic). Returns
 *  the number of blocks processed. */
int dsp_blockfifo_process(dsp_blockfifo_t *f, const int16_t *mic, const int16_t *ref, int16_t *out,
                          dsp_block_fn_t fn, void *ctx);

/** Sum of squares (energy) of n samples. */
uint64_t dsp_energy(const int16_t *x, size_t n);
/** 10*log10(num/den) in dB; 0 when either is 0. */
float dsp_ratio_db(uint64_t num, uint64_t den);
