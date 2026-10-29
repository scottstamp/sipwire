#include "audio_dsp.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/* helpers                                                            */
/* ------------------------------------------------------------------ */

static inline int16_t sat16(int32_t v)
{
    return v > INT16_MAX ? INT16_MAX : (v < INT16_MIN ? INT16_MIN : (int16_t)v);
}

/* ------------------------------------------------------------------ */
/* DC blocker                                                         */
/* ------------------------------------------------------------------ */

void dsp_dcblock_init(dsp_dcblock_t *s, uint32_t fs_hz, uint32_t fc_hz)
{
    /* R = 1 - 2*pi*fc/fs, in Q15, without libm (2*pi*32768 = 205887) */
    uint64_t drop = (uint64_t)205887 * fc_hz / (fs_hz ? fs_hz : 1);
    s->r_q15 = drop >= 32768 ? 0 : (int32_t)(32768 - drop);
    s->px = 0;
    s->y8 = 0;
}

int16_t dsp_dcblock_step(dsp_dcblock_t *s, int32_t x)
{
    int64_t acc = (int64_t)(x - s->px) * 256 + (((int64_t)s->y8 * s->r_q15) >> 15);
    if (acc > (1 << 30)) {
        acc = 1 << 30;
    } else if (acc < -(1 << 30)) {
        acc = -(1 << 30);
    }
    s->px = x;
    s->y8 = (int32_t)acc;
    return sat16((s->y8 + 128) >> 8);
}

void dsp_dcblock_process(dsp_dcblock_t *s, const int32_t *in, int16_t *out, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        out[i] = dsp_dcblock_step(s, in[i]);
    }
}

/* ------------------------------------------------------------------ */
/* Gain + soft clip                                                   */
/* ------------------------------------------------------------------ */

int32_t dsp_gain_q15(uint8_t volume)
{
    uint32_t v = volume > 100 ? 100 : volume;
    return (int32_t)((v * v * DSP_GAIN_UNITY + 5000) / 10000);
}

void dsp_gain_ramp(int32_t *buf, size_t n, int32_t g0, int32_t g1)
{
    if (n == 0) {
        return;
    }
    if (g0 == g1) {
        if (g0 != DSP_GAIN_UNITY) {
            for (size_t i = 0; i < n; i++) {
                buf[i] = (int32_t)(((int64_t)buf[i] * g0) >> 15);
            }
        }
        return;
    }
    for (size_t i = 0; i < n; i++) {
        int32_t g = g0 + (int32_t)(((int64_t)(g1 - g0) * (int64_t)(i + 1)) / (int64_t)n);
        buf[i] = (int32_t)(((int64_t)buf[i] * g) >> 15);
    }
}

#define CLIP_KNEE  24576                    /* 0.75 FS */
#define CLIP_RANGE (32766 - CLIP_KNEE)

int16_t dsp_softclip(int32_t x)
{
    int64_t ax = x < 0 ? -(int64_t)x : (int64_t)x;
    if (ax <= CLIP_KNEE) {
        return (int16_t)x;
    }
    /* knee: y = T + R*e/(e+R): slope 1 at e = 0, asymptote T+R < 32767 */
    int64_t e = ax - CLIP_KNEE;
    int32_t y = CLIP_KNEE + (int32_t)((e * CLIP_RANGE) / (e + CLIP_RANGE));
    return (int16_t)(x < 0 ? -y : y);
}

void dsp_softclip_buf(const int32_t *in, int16_t *out, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        out[i] = dsp_softclip(in[i]);
    }
}

/* ------------------------------------------------------------------ */
/* Tone generator                                                     */
/* ------------------------------------------------------------------ */

static const int16_t SINE[256] = {
    0, 804, 1608, 2410, 3212, 4011, 4808, 5602, 6393, 7179, 7962, 8739,
    9512, 10278, 11039, 11793, 12539, 13279, 14010, 14732, 15446, 16151, 16846, 17530,
    18204, 18868, 19519, 20159, 20787, 21403, 22005, 22594, 23170, 23731, 24279, 24811,
    25329, 25832, 26319, 26790, 27245, 27683, 28105, 28510, 28898, 29268, 29621, 29956,
    30273, 30571, 30852, 31113, 31356, 31580, 31785, 31971, 32137, 32285, 32412, 32521,
    32609, 32678, 32728, 32757, 32767, 32757, 32728, 32678, 32609, 32521, 32412, 32285,
    32137, 31971, 31785, 31580, 31356, 31113, 30852, 30571, 30273, 29956, 29621, 29268,
    28898, 28510, 28105, 27683, 27245, 26790, 26319, 25832, 25329, 24811, 24279, 23731,
    23170, 22594, 22005, 21403, 20787, 20159, 19519, 18868, 18204, 17530, 16846, 16151,
    15446, 14732, 14010, 13279, 12539, 11793, 11039, 10278, 9512, 8739, 7962, 7179,
    6393, 5602, 4808, 4011, 3212, 2410, 1608, 804, 0, -804, -1608, -2410,
    -3212, -4011, -4808, -5602, -6393, -7179, -7962, -8739, -9512, -10278, -11039, -11793,
    -12539, -13279, -14010, -14732, -15446, -16151, -16846, -17530, -18204, -18868, -19519, -20159,
    -20787, -21403, -22005, -22594, -23170, -23731, -24279, -24811, -25329, -25832, -26319, -26790,
    -27245, -27683, -28105, -28510, -28898, -29268, -29621, -29956, -30273, -30571, -30852, -31113,
    -31356, -31580, -31785, -31971, -32137, -32285, -32412, -32521, -32609, -32678, -32728, -32757,
    -32767, -32757, -32728, -32678, -32609, -32521, -32412, -32285, -32137, -31971, -31785, -31580,
    -31356, -31113, -30852, -30571, -30273, -29956, -29621, -29268, -28898, -28510, -28105, -27683,
    -27245, -26790, -26319, -25832, -25329, -24811, -24279, -23731, -23170, -22594, -22005, -21403,
    -20787, -20159, -19519, -18868, -18204, -17530, -16846, -16151, -15446, -14732, -14010, -13279,
    -12539, -11793, -11039, -10278, -9512, -8739, -7962, -7179, -6393, -5602, -4808, -4011,
    -3212, -2410, -1608, -804,
};

#define TONE_AMP     9830               /* 0.30 FS per component */
#define TONE_RAMP_MS 5

static inline int32_t sine_at(uint32_t phase)
{
    uint32_t i = phase >> 24;
    int32_t a = SINE[i];
    int32_t b = SINE[(i + 1) & 255];
    int32_t frac = (int32_t)((phase >> 8) & 0xFFFF);
    return a + (int32_t)(((int64_t)(b - a) * frac) >> 16);
}

static uint32_t freq_to_inc(uint32_t hz, uint32_t rate)
{
    return (uint32_t)(((uint64_t)hz << 32) / rate);
}

static uint32_t ms_to_n(uint32_t ms, uint32_t rate)
{
    return (uint32_t)((uint64_t)ms * rate / 1000);
}

static const dsp_tone_def_t PRESETS[] = {
    [DSP_TONE_DIAL]     = { .f1 = 350,  .f2 = 440 },
    [DSP_TONE_RINGBACK] = { .f1 = 440,  .f2 = 480,  .on_ms = 2000, .period_ms = 6000 },
    [DSP_TONE_BUSY]     = { .f1 = 480,  .f2 = 620,  .on_ms = 500,  .period_ms = 1000 },
    [DSP_TONE_RING]     = { .f1 = 1000, .f2 = 1200, .on_ms = 1000, .period_ms = 3000, .warble_ms = 40 },
    [DSP_TONE_KEYCLICK] = { .f1 = 1000, .f2 = 2000, .on_ms = 30, .oneshot = true },
    [DSP_TONE_CALL_WAITING] = { .f1 = 440, .on_ms = 300, .period_ms = 10000 },
};

#define S(a, b, ms) { a, b, ms }
#define R(ms) { 0, 0, ms }
static const dsp_step_t RT_CLASSIC[] = { S(950, 1250, 400), R(200), S(950, 1250, 400), R(2000) };
static const dsp_step_t RT_TRILL[] = {
    S(1500, 0, 60), S(1900, 0, 60), S(1500, 0, 60), S(1900, 0, 60), S(1500, 0, 60), S(1900, 0, 60),
    S(1500, 0, 60), S(1900, 0, 60), R(2000),
};
static const dsp_step_t RT_CHIME[] = { S(1047, 0, 300), S(1319, 0, 300), S(1568, 0, 500), R(1900) };
static const dsp_step_t RT_ASCEND[] = {
    S(523, 0, 150), R(10), S(659, 0, 150), R(10), S(784, 0, 150), R(10), S(1047, 0, 250), R(1500),
};
static const dsp_step_t RT_BEEPS[] = {
    S(1400, 0, 100), R(100), S(1400, 0, 100), R(100), S(1400, 0, 100), R(1700),
};
static const dsp_step_t RT_BELL[] = { S(784, 1568, 600), R(300), S(659, 1319, 900), R(2000) };
#undef S
#undef R

static const struct { const char *name; const dsp_step_t *steps; uint8_t n; } RINGTONES[] = {
#define RT(nm, arr) { nm, arr, (uint8_t)(sizeof(arr) / sizeof(arr[0])) }
    RT("Classic", RT_CLASSIC), RT("Trill", RT_TRILL), RT("Chime", RT_CHIME),
    RT("Ascending", RT_ASCEND), RT("Beeps", RT_BEEPS), RT("Bell", RT_BELL),
#undef RT
};

int dsp_ringtone_count(void)
{
    return (int)(sizeof(RINGTONES) / sizeof(RINGTONES[0]));
}

const char *dsp_ringtone_name(int idx)
{
    return idx >= 0 && idx < dsp_ringtone_count() ? RINGTONES[idx].name : NULL;
}

static void seq_load(dsp_tone_t *t)
{
    const dsp_step_t *st = &t->seq[t->seq_i];
    t->inc1 = freq_to_inc(st->f1, t->rate);
    t->inc2 = freq_to_inc(st->f2, t->rate);
    t->single = st->f2 == 0;
    t->seq_pos = 0;
}

void dsp_tone_start_ringtone(dsp_tone_t *t, int idx)
{
    if (idx < 0 || idx >= dsp_ringtone_count()) {
        idx = 0;
    }
    dsp_tone_def_t d = { .f1 = 1000 };          /* resets cadence state; the sequence overrides */
    dsp_tone_start_def(t, &d);
    t->on_n = t->period_n = 0;
    t->seq = RINGTONES[idx].steps;
    t->seq_n = RINGTONES[idx].n;
    t->seq_i = 0;
    seq_load(t);
}

static const char DTMF_KEYS[16] = {
    '1', '2', '3', 'A', '4', '5', '6', 'B', '7', '8', '9', 'C', '*', '0', '#', 'D',
};
static const uint16_t DTMF_ROW[4] = { 697, 770, 852, 941 };
static const uint16_t DTMF_COL[4] = { 1209, 1336, 1477, 1633 };

bool dsp_dtmf_freqs(char digit, unsigned *lo, unsigned *hi)
{
    if (digit >= 'a' && digit <= 'd') {
        digit = (char)(digit - 'a' + 'A');
    }
    for (int i = 0; i < 16; i++) {
        if (DTMF_KEYS[i] == digit) {
            if (lo) {
                *lo = DTMF_ROW[i / 4];
            }
            if (hi) {
                *hi = DTMF_COL[i % 4];
            }
            return true;
        }
    }
    return false;
}

void dsp_tone_init(dsp_tone_t *t, uint32_t rate)
{
    uint32_t ramp = ms_to_n(TONE_RAMP_MS, rate);

    memset(t, 0, sizeof(*t));
    t->rate = rate;
    t->amp = TONE_AMP;
    t->env_step = DSP_GAIN_UNITY / (int32_t)(ramp ? ramp : 1);
}

void dsp_tone_start_def(dsp_tone_t *t, const dsp_tone_def_t *def)
{
    /* phases and envelope are kept: switching tones doesn't click */
    t->inc1 = freq_to_inc(def->f1, t->rate);
    t->inc2 = freq_to_inc(def->f2, t->rate);
    t->single = def->f2 == 0;
    t->on_n = ms_to_n(def->on_ms, t->rate);
    t->oneshot = def->oneshot && t->on_n > 0;
    t->period_n = (t->on_n == 0 || t->oneshot) ? 0 : ms_to_n(def->period_ms, t->rate);
    if (t->on_n > 0 && !t->oneshot && t->period_n <= t->on_n) {
        t->on_n = 0;                            /* degenerate cadence: continuous */
        t->period_n = 0;
    }
    t->warble_n = ms_to_n(def->warble_ms, t->rate);
    t->warble_pos = 0;
    t->warble_hi = false;
    t->pos = 0;
    t->seq = NULL;
    t->stopping = false;
    t->active = true;
}

void dsp_tone_start(dsp_tone_t *t, dsp_tone_kind_t kind)
{
    if ((size_t)kind < sizeof(PRESETS) / sizeof(PRESETS[0])) {
        dsp_tone_start_def(t, &PRESETS[kind]);
    }
}

bool dsp_tone_start_dtmf(dsp_tone_t *t, char digit)
{
    unsigned lo, hi;
    if (!dsp_dtmf_freqs(digit, &lo, &hi)) {
        return false;
    }
    dsp_tone_def_t def = { .f1 = (uint16_t)lo, .f2 = (uint16_t)hi, .on_ms = 100, .oneshot = true };
    dsp_tone_start_def(t, &def);
    return true;
}

void dsp_tone_stop(dsp_tone_t *t)
{
    if (t->active) {
        t->stopping = true;
    }
}

bool dsp_tone_active(const dsp_tone_t *t)
{
    return t->active;
}

void dsp_tone_add(dsp_tone_t *t, int32_t *acc, size_t n)
{
    for (size_t i = 0; i < n && t->active; i++) {
        bool on;
        if (t->stopping) {
            on = false;
        } else if (t->seq) {
            on = t->seq[t->seq_i].f1 != 0;
        } else if (t->on_n == 0) {
            on = true;                          /* continuous */
        } else {
            on = t->pos < t->on_n;
        }

        if (on) {
            t->env += t->env_step;
            if (t->env > DSP_GAIN_UNITY) {
                t->env = DSP_GAIN_UNITY;
            }
        } else {
            t->env -= t->env_step;
            if (t->env < 0) {
                t->env = 0;
            }
        }

        if (t->env > 0) {
            int32_t s;
            if (t->warble_n > 0) {
                /* one oscillator hopping between f1 and f2, phase-continuous */
                s = (sine_at(t->ph1) * (t->amp * 2)) >> 15;
                t->ph1 += t->warble_hi ? t->inc2 : t->inc1;
                if (++t->warble_pos >= t->warble_n) {
                    t->warble_pos = 0;
                    t->warble_hi = !t->warble_hi;
                }
            } else if (t->single) {
                s = (sine_at(t->ph1) * (t->amp * 2)) >> 15;
                t->ph1 += t->inc1;
            } else {
                s = ((sine_at(t->ph1) + sine_at(t->ph2)) * t->amp) >> 15;
                t->ph1 += t->inc1;
                t->ph2 += t->inc2;
            }
            acc[i] += (int32_t)(((int64_t)s * t->env) >> 15);
        }

        /* cadence position */
        if (t->seq) {
            if (++t->seq_pos >= ms_to_n(t->seq[t->seq_i].ms, t->rate)) {
                t->seq_i = (uint8_t)((t->seq_i + 1u) % t->seq_n);
                seq_load(t);
            }
        } else if (t->period_n > 0) {
            if (++t->pos >= t->period_n) {
                t->pos = 0;
            }
        } else if (t->on_n > 0 && t->pos < UINT32_MAX) {
            t->pos++;                           /* one-shot: runs past on_n */
        }

        if (t->env == 0 && (t->stopping || (t->oneshot && t->pos >= t->on_n))) {
            t->active = false;
        }
    }
}
