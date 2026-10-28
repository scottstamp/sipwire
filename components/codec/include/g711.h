/* G.711 mu-law / A-law companding (ITU-T G.711). Pure C, no IDF deps.
 * PCM is signed 16-bit linear, 8 kHz. Codes are the on-wire bytes. */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint8_t g711_ulaw_encode(int16_t pcm);
int16_t g711_ulaw_decode(uint8_t code);
uint8_t g711_alaw_encode(int16_t pcm);
int16_t g711_alaw_decode(uint8_t code);

void g711_ulaw_encode_buf(const int16_t *pcm, uint8_t *out, size_t n);
void g711_ulaw_decode_buf(const uint8_t *in, int16_t *pcm, size_t n);
void g711_alaw_encode_buf(const int16_t *pcm, uint8_t *out, size_t n);
void g711_alaw_decode_buf(const uint8_t *in, int16_t *pcm, size_t n);

#ifdef __cplusplus
}
#endif
