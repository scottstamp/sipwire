/* Half-band 2:1 / 1:2 resampler. See resample2.h. */

#include "resample2.h"

#include <string.h>

static inline int16_t sat16(int32_t v)
{
    return v > INT16_MAX ? INT16_MAX : (v < INT16_MIN ? INT16_MIN : (int16_t)v);
}

/* 47-tap Kaiser(beta 5) half-band low-pass, fc = fs/4, Q15, DC gain 1.
 * Taps at even offsets from the centre are zero; the centre tap is
 * HB_CENTER; HB_ODD[p] is the tap at offset +/-(2p+1). */
#define HB_CENTER 16388
#define HB_NODD   12
static const int32_t HB_ODD[HB_NODD] = {
    10389, -3347, 1875, -1207, 813, -553, 371, -241, 149, -85, 43, -17,
};

#define RS_CHUNK 256                /* samples per internal block */

void resample2_down_init(resample2_down_t *d)
{
    memset(d, 0, sizeof(*d));
}

void resample2_up_init(resample2_up_t *u)
{
    memset(u, 0, sizeof(*u));
}

void resample2_down(resample2_down_t *d, const int16_t *in, size_t n_out, int16_t *out)
{
    int16_t buf[RESAMPLE2_DOWN_HIST + RS_CHUNK];

    while (n_out > 0) {
        size_t no = n_out > RS_CHUNK / 2 ? RS_CHUNK / 2 : n_out;
        size_t ni = no * 2;

        memcpy(buf, d->hist, sizeof(d->hist));
        memcpy(buf + RESAMPLE2_DOWN_HIST, in, ni * sizeof(int16_t));

        for (size_t j = 0; j < no; j++) {
            /* newest sample for this output is buf[46 + 2j]; centre tap is 23 back */
            const int16_t *c = &buf[RESAMPLE2_DOWN_HIST + 2 * j - RESAMPLE2_UP_HIST];
            int32_t acc = HB_CENTER * c[0];
            for (int p = 0; p < HB_NODD; p++) {
                acc += HB_ODD[p] * ((int32_t)c[-(2 * p + 1)] + c[2 * p + 1]);
            }
            out[j] = sat16((acc + 16384) >> 15);
        }

        memcpy(d->hist, buf + ni, sizeof(d->hist));
        in += ni;
        out += no;
        n_out -= no;
    }
}

void resample2_up(resample2_up_t *u, const int16_t *in, size_t n_in, int16_t *out)
{
    int16_t buf[RESAMPLE2_UP_HIST + RS_CHUNK];

    while (n_in > 0) {
        size_t n = n_in > RS_CHUNK ? RS_CHUNK : n_in;

        memcpy(buf, u->hist, sizeof(u->hist));
        memcpy(buf + RESAMPLE2_UP_HIST, in, n * sizeof(int16_t));

        for (size_t i = 0; i < n; i++) {
            /* x[n-k] = buf[23 + i - k]. Even output: polyphase of the odd taps
             * (x[n-j] pairs with x[n-23+j]); odd output: the centre tap = x[n-11]. */
            const int16_t *x = &buf[RESAMPLE2_UP_HIST + i];
            int32_t acc = 0;
            for (int j = 0; j < HB_NODD; j++) {
                acc += HB_ODD[HB_NODD - 1 - j] * ((int32_t)x[-j] + x[-(RESAMPLE2_UP_HIST - j)]);
            }
            out[2 * i] = sat16((acc + 8192) >> 14);     /* x2 interpolation gain */
            out[2 * i + 1] = x[-11];
        }

        memcpy(u->hist, buf + n, sizeof(u->hist));
        in += n;
        out += 2 * n;
        n_in -= n;
    }
}
