/* ITU-T G.722 encoder/decoder, 64 kbit/s only. See g722.h.
 *
 * Provenance: adapted from the G.722 codec shipped with Asterisk
 * (codecs/g722/g722_encode.c, g722_decode.c), which is SpanDSP code
 *
 *     Written by Steve Underwood <steveu@coppice.org>
 *     Copyright (C) 2005 Steve Underwood
 *     "Despite my general liking of the GPL, I place my own contributions
 *      to this code in the public domain for the benefit of all mankind -
 *      even the slimy ones who might try to proprietize my work and use it
 *      to my detriment."
 *
 *     Based on a single channel 64kbps only G.722 codec which is:
 *     Copyright (c) CMU 1993, Computer Science, Speech Group,
 *     Chengxiang Lu and Alex Hauptmann
 *
 * Changes for sipwire: 64 kbit/s mode only (no 56/48 kbit/s, no packing,
 * no 8 kHz shortcut, no ITU test mode), caller-owned state without malloc,
 * fixed-width types, one shared block 4, no left shifts of negative values
 * (UBSan), and the decoder's QMF output is saturated instead of wrapping.
 * The arithmetic is otherwise unchanged, so the bitstream is identical to
 * Asterisk's codec_g722 (checked by the host tests via Asterisk's own
 * "file convert"). */

#include "g722.h"

#include <string.h>

static inline int32_t sat16(int32_t v)
{
    return v > INT16_MAX ? INT16_MAX : (v < INT16_MIN ? INT16_MIN : v);
}

/* Shared tables (encoder and decoder). */
static const int32_t k_wl[8] = {-60, -30, 58, 172, 334, 538, 1198, 3042};
static const int32_t k_rl42[16] = {0, 7, 6, 5, 4, 3, 2, 1, 7, 6, 5, 4, 3, 2, 1, 0};
static const int32_t k_ilb[32] = {
    2048, 2093, 2139, 2186, 2233, 2282, 2332, 2383, 2435, 2489, 2543,
    2599, 2656, 2714, 2774, 2834, 2896, 2960, 3025, 3091, 3158, 3228,
    3298, 3371, 3444, 3520, 3597, 3676, 3756, 3838, 3922, 4008,
};
static const int32_t k_qm4[16] = {
    0,     -20456, -12896, -8968, -6288, -4240, -2584, -1200,
    20456, 12896,  8968,   6288,  4240,  2584,  1200,  0,
};
static const int32_t k_qm2[4] = {-7408, -1616, 7408, 1616};
static const int32_t k_wh[3] = {0, -214, 798};
static const int32_t k_rh2[4] = {2, 1, 2, 1};
static const int32_t k_qmf[12] = {3, -11, 12, 32, -210, 951, 3876, -805, 362, -156, 53, -11};

/* Block 4: reconstruction, pole/zero predictor adaptation and prediction. */
static void block4(g722_band_t *b, int32_t d)
{
    int32_t sg[7];
    int32_t ap[3];
    int32_t wd1, wd2, wd3;

    /* RECONS, PARREC */
    b->d[0] = d;
    b->r[0] = sat16(b->s + d);
    b->p[0] = sat16(b->sz + d);

    /* UPPOL2 */
    for (int i = 0; i < 3; i++) {
        sg[i] = b->p[i] >> 15;
    }
    wd1 = sat16(b->a[1] * 4);
    wd2 = (sg[0] == sg[1]) ? -wd1 : wd1;
    if (wd2 > 32767) {
        wd2 = 32767;
    }
    wd3 = (wd2 >> 7) + ((sg[0] == sg[2]) ? 128 : -128);
    wd3 += (b->a[2] * 32512) >> 15;
    if (wd3 > 12288) {
        wd3 = 12288;
    } else if (wd3 < -12288) {
        wd3 = -12288;
    }
    ap[2] = wd3;

    /* UPPOL1 */
    wd1 = (sg[0] == sg[1]) ? 192 : -192;
    wd2 = (b->a[1] * 32640) >> 15;
    ap[1] = sat16(wd1 + wd2);
    wd3 = sat16(15360 - ap[2]);
    if (ap[1] > wd3) {
        ap[1] = wd3;
    } else if (ap[1] < -wd3) {
        ap[1] = -wd3;
    }

    /* UPZERO */
    wd1 = (d == 0) ? 0 : 128;
    sg[0] = d >> 15;
    int32_t bp[7];
    for (int i = 1; i < 7; i++) {
        sg[i] = b->d[i] >> 15;
        wd2 = (sg[i] == sg[0]) ? wd1 : -wd1;
        wd3 = (b->b[i] * 32640) >> 15;
        bp[i] = sat16(wd2 + wd3);
    }

    /* DELAYA */
    for (int i = 6; i > 0; i--) {
        b->d[i] = b->d[i - 1];
        b->b[i] = bp[i];
    }
    for (int i = 2; i > 0; i--) {
        b->r[i] = b->r[i - 1];
        b->p[i] = b->p[i - 1];
        b->a[i] = ap[i];
    }

    /* FILTEP */
    wd1 = sat16(b->r[1] + b->r[1]);
    wd1 = (b->a[1] * wd1) >> 15;
    wd2 = sat16(b->r[2] + b->r[2]);
    wd2 = (b->a[2] * wd2) >> 15;
    b->sp = sat16(wd1 + wd2);

    /* FILTEZ */
    b->sz = 0;
    for (int i = 6; i > 0; i--) {
        wd1 = sat16(b->d[i] + b->d[i]);
        b->sz += (b->b[i] * wd1) >> 15;
    }
    b->sz = sat16(b->sz);

    /* PREDIC */
    b->s = sat16(b->sp + b->sz);
}

/* Blocks 3L/3H: LOGSCL/LOGSCH + SCALEL/SCALEH. */
static void scale_update(g722_band_t *b, int32_t w, int32_t nb_max, int32_t shift)
{
    int32_t nb = ((b->nb * 127) >> 7) + w;
    if (nb < 0) {
        nb = 0;
    } else if (nb > nb_max) {
        nb = nb_max;
    }
    b->nb = nb;
    int32_t wd1 = (nb >> 6) & 31;
    int32_t wd2 = shift - (nb >> 11);
    int32_t wd3 = (wd2 < 0) ? (k_ilb[wd1] << -wd2) : (k_ilb[wd1] >> wd2);
    b->det = wd3 << 2;
}

static void band_init(g722_band_t *band)
{
    memset(band, 0, 2 * sizeof(*band));
    band[0].det = 32;
    band[1].det = 8;
}

void g722_enc_init(g722_enc_t *e)
{
    memset(e->x, 0, sizeof(e->x));
    band_init(e->band);
}

void g722_dec_init(g722_dec_t *d)
{
    memset(d->x, 0, sizeof(d->x));
    band_init(d->band);
}

size_t g722_encode(g722_enc_t *e, const int16_t *pcm, size_t n_samples, uint8_t *out)
{
    static const int32_t q6[32] = {
        0,   35,  72,  110, 150, 190,  233,  276,  323,  370,  422,  473,  530,  587,  650,  714,
        786, 858, 940, 1023, 1121, 1219, 1339, 1458, 1612, 1765, 1980, 2195, 2557, 2919, 0, 0,
    };
    static const int32_t iln[32] = {
        0,  63, 62, 31, 30, 29, 28, 27, 26, 25, 24, 23, 22, 21, 20, 19,
        18, 17, 16, 15, 14, 13, 12, 11, 10, 9,  8,  7,  6,  5,  4,  0,
    };
    static const int32_t ilp[32] = {
        0,  61, 60, 59, 58, 57, 56, 55, 54, 53, 52, 51, 50, 49, 48, 47,
        46, 45, 44, 43, 42, 41, 40, 39, 38, 37, 36, 35, 34, 33, 32, 0,
    };
    static const int32_t ihn[3] = {0, 1, 0};
    static const int32_t ihp[3] = {0, 3, 2};

    size_t n_out = n_samples / 2;
    for (size_t j = 0; j < n_out; j++) {
        /* Transmit QMF: split into two 8 kHz sub-bands. */
        memmove(e->x, e->x + 2, 22 * sizeof(e->x[0]));
        e->x[22] = pcm[2 * j];
        e->x[23] = pcm[2 * j + 1];
        int32_t sumeven = 0, sumodd = 0;
        for (int i = 0; i < 12; i++) {
            sumodd += e->x[2 * i] * k_qmf[i];
            sumeven += e->x[2 * i + 1] * k_qmf[11 - i];
        }
        int32_t xlow = (sumeven + sumodd) >> 14;
        int32_t xhigh = (sumeven - sumodd) >> 14;

        /* Low band. 1L SUBTRA, QUANTL */
        g722_band_t *lo = &e->band[0];
        int32_t el = sat16(xlow - lo->s);
        int32_t wd = (el >= 0) ? el : -(el + 1);
        int i;
        for (i = 1; i < 30; i++) {
            if (wd < ((q6[i] * lo->det) >> 12)) {
                break;
            }
        }
        int32_t ilow = (el < 0) ? iln[i] : ilp[i];
        /* 2L INVQAL, 3L, 4L */
        int32_t ril = ilow >> 2;
        int32_t dlow = (lo->det * k_qm4[ril]) >> 15;
        scale_update(lo, k_wl[k_rl42[ril]], 18432, 8);
        block4(lo, dlow);

        /* High band. 1H SUBTRA, QUANTH */
        g722_band_t *hi = &e->band[1];
        int32_t eh = sat16(xhigh - hi->s);
        wd = (eh >= 0) ? eh : -(eh + 1);
        int32_t mih = (wd >= ((564 * hi->det) >> 12)) ? 2 : 1;
        int32_t ihigh = (eh < 0) ? ihn[mih] : ihp[mih];
        /* 2H INVQAH, 3H, 4H */
        int32_t dhigh = (hi->det * k_qm2[ihigh]) >> 15;
        scale_update(hi, k_wh[k_rh2[ihigh]], 22528, 10);
        block4(hi, dhigh);

        out[j] = (uint8_t)((ihigh << 6) | ilow);
    }
    return n_out;
}

size_t g722_decode(g722_dec_t *d, const uint8_t *in, size_t n_bytes, int16_t *pcm)
{
    static const int32_t qm6[64] = {
        -136,   -136,   -136,   -136,   -24808, -21904, -19008, -16704,
        -14984, -13512, -12280, -11192, -10232, -9360,  -8576,  -7856,
        -7192,  -6576,  -6000,  -5456,  -4944,  -4464,  -4008,  -3576,
        -3168,  -2776,  -2400,  -2032,  -1688,  -1360,  -1040,  -728,
        24808,  21904,  19008,  16704,  14984,  13512,  12280,  11192,
        10232,  9360,   8576,   7856,   7192,   6576,   6000,   5456,
        4944,   4464,   4008,   3576,   3168,   2776,   2400,   2032,
        1688,   1360,   1040,   728,    432,    136,    -432,   -136,
    };

    for (size_t j = 0; j < n_bytes; j++) {
        int32_t code = in[j];
        int32_t ilow = code & 0x3F;
        int32_t ihigh = (code >> 6) & 0x03;

        /* Low band. 5L INVQBL, RECONS, 6L LIMIT */
        g722_band_t *lo = &d->band[0];
        int32_t rlow = lo->s + ((lo->det * qm6[ilow]) >> 15);
        if (rlow > 16383) {
            rlow = 16383;
        } else if (rlow < -16384) {
            rlow = -16384;
        }
        /* 2L INVQAL (4-bit truncated code), 3L, 4L */
        int32_t ril = ilow >> 2;
        int32_t dlowt = (lo->det * k_qm4[ril]) >> 15;
        scale_update(lo, k_wl[k_rl42[ril]], 18432, 8);
        block4(lo, dlowt);

        /* High band. 2H INVQAH, 5H RECONS, 6H LIMIT, 3H, 4H */
        g722_band_t *hi = &d->band[1];
        int32_t dhigh = (hi->det * k_qm2[ihigh]) >> 15;
        int32_t rhigh = dhigh + hi->s;
        if (rhigh > 16383) {
            rhigh = 16383;
        } else if (rhigh < -16384) {
            rhigh = -16384;
        }
        scale_update(hi, k_wh[k_rh2[ihigh]], 22528, 10);
        block4(hi, dhigh);

        /* Receive QMF: recombine the sub-bands at 16 kHz. */
        memmove(d->x, d->x + 2, 22 * sizeof(d->x[0]));
        d->x[22] = rlow + rhigh;
        d->x[23] = rlow - rhigh;
        int32_t xout1 = 0, xout2 = 0;
        for (int i = 0; i < 12; i++) {
            xout2 += d->x[2 * i] * k_qmf[i];
            xout1 += d->x[2 * i + 1] * k_qmf[11 - i];
        }
        pcm[2 * j] = (int16_t)sat16(xout1 >> 11);
        pcm[2 * j + 1] = (int16_t)sat16(xout2 >> 11);
    }
    return 2 * n_bytes;
}
