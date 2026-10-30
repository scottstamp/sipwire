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
    DSP_TONE_CALL_WAITING,
    DSP_TONE_WEAK_SIGNAL,           /* two short high beeps, once (out-of-range warning) */
} dsp_tone_kind_t;

typedef struct {
    uint16_t f1, f2;                /* Hz; f2 == 0: single tone */
    uint16_t on_ms;                 /* 0: continuous */
    uint16_t period_ms;             /* on + off; ignored when continuous or one-shot */
    uint16_t warble_ms;             /* > 0: alternate f1/f2 every warble_ms (single oscillator) */
    bool oneshot;                   /* play on_ms once, then go inactive */
    uint8_t count;                  /* oneshot with count > 1: count bursts of on_ms every
                                     * period_ms, then inactive */
} dsp_tone_def_t;

typedef struct {
    uint32_t rate;
    uint32_t ph1, ph2, inc1, inc2;
    uint32_t on_n, period_n, pos;   /* samples */
    uint32_t warble_n, warble_pos;
    bool warble_hi;
    bool oneshot, active, stopping, single;
    uint8_t bursts;                 /* bursts left of a counted one-shot, 0 = not counted */
    int32_t env, env_step;          /* Q15 attack/release envelope */
    int32_t amp;                    /* Q15 per component */
    /* step sequence (ringtones): repeats forever, takes over from the cadence fields */
    const struct dsp_step *seq;
    uint8_t seq_n, seq_i;
    uint32_t seq_pos;               /* samples into the current step */
} dsp_tone_t;

/** One note of a ringtone: f1 == 0 is silence; f2 == 0 a single sine. */
typedef struct dsp_step {
    uint16_t f1, f2;
    uint16_t ms;
} dsp_step_t;

/** Built-in ringtones (each is a loop incl. its trailing pause). */
int dsp_ringtone_count(void);
const char *dsp_ringtone_name(int idx);
/** Start ringtone idx (out of range -> 0). */
void dsp_tone_start_ringtone(dsp_tone_t *t, int idx);

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
