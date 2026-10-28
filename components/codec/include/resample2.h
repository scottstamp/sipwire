/* 2:1 / 1:2 sample-rate conversion with a half-band FIR (47 taps, Kaiser
 * beta 5, ~-39 dB at 4.5 kHz @ 16 kHz). Pure C, no IDF deps, no allocation.
 * Bridges the 16 kHz audio path and 8 kHz narrowband codecs (G.711):
 * decimate 16 -> 8 kHz before encoding, interpolate 8 -> 16 kHz after
 * decoding. Group delay: 23 samples at 16 kHz per direction.
 * Each state struct is owned by one stream. */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RESAMPLE2_DOWN_HIST 46      /* taps - 1 */
#define RESAMPLE2_UP_HIST   23      /* (taps - 1) / 2 */

typedef struct { int16_t hist[RESAMPLE2_DOWN_HIST]; } resample2_down_t;
typedef struct { int16_t hist[RESAMPLE2_UP_HIST]; } resample2_up_t;

void resample2_down_init(resample2_down_t *d);
void resample2_up_init(resample2_up_t *u);

/** Low-pass and drop every other sample. Consumes 2*n_out input samples. */
void resample2_down(resample2_down_t *d, const int16_t *in, size_t n_out, int16_t *out);
/** Zero-stuff and low-pass (unity gain). Produces 2*n_in output samples. */
void resample2_up(resample2_up_t *u, const int16_t *in, size_t n_in, int16_t *out);

#ifdef __cplusplus
}
#endif
