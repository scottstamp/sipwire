/* ITU-T G.722 wideband codec, 64 kbit/s mode only (mode 1: 6+2 bits per
 * sample pair). Pure C, no IDF deps, no allocation: the caller owns the state.
 *
 * PCM is signed 16-bit linear at 16 kHz; every pair of input samples becomes
 * one code byte (low band in bits 0-5, high band in bits 6-7), so 20 ms =
 * 320 samples = 160 bytes. The RTP clock rate of G.722 is nevertheless 8000
 * (RFC 3551 4.5.2): that is the RTP layer's business, not the codec's.
 *
 * Bitstream compatible with Asterisk/spandsp G.722 (same algorithm); see
 * g722.c for provenance. */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ADPCM state of one sub-band (G.722 blocks 2-4). */
typedef struct {
    int32_t s;                  /* predicted signal */
    int32_t sp;                 /* pole section output */
    int32_t sz;                 /* zero section output */
    int32_t r[3];               /* reconstructed signal history */
    int32_t a[3];               /* pole predictor coefficients */
    int32_t p[3];               /* partial reconstructed signal history */
    int32_t d[7];               /* quantized difference history */
    int32_t b[7];               /* zero predictor coefficients */
    int32_t nb;                 /* log scale factor */
    int32_t det;                /* quantizer scale factor */
} g722_band_t;

typedef struct {
    int32_t x[24];              /* QMF analysis history */
    g722_band_t band[2];        /* 0 = low, 1 = high */
} g722_enc_t;

typedef struct {
    int32_t x[24];              /* QMF synthesis history */
    g722_band_t band[2];
} g722_dec_t;

/** Reset to the G.722 initial state (also the "reset" of a running codec). */
void g722_enc_init(g722_enc_t *e);
void g722_dec_init(g722_dec_t *d);

/** Encode n_samples (even) of 16 kHz PCM into n_samples/2 bytes. Returns the
 *  number of bytes written. An odd trailing sample is ignored. */
size_t g722_encode(g722_enc_t *e, const int16_t *pcm, size_t n_samples, uint8_t *out);

/** Decode n_bytes into 2*n_bytes samples of 16 kHz PCM. Returns the number
 *  of samples written. */
size_t g722_decode(g722_dec_t *d, const uint8_t *in, size_t n_bytes, int16_t *pcm);

#ifdef __cplusplus
}
#endif
