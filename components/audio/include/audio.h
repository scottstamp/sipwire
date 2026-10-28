#pragma once

/* I2S speaker + mic. 16 kHz mono 16-bit at the API (same rate as I2S, no
 * resampling here), 20 ms frames. Narrowband codecs resample in rtp.
 * Owns two tasks (core 1): TX pulls frames from a source callback, RX pushes
 * mic frames to a sink callback. Callbacks run in those tasks: keep them
 * short, non-blocking (rtp pull/build under a mutex is fine). */

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define AUDIO_SAMPLE_RATE   16000
#define AUDIO_FRAME_SAMPLES 320     /* 20 ms */

typedef void (*audio_source_cb_t)(int16_t frame[AUDIO_FRAME_SAMPLES], void *ctx);
typedef void (*audio_sink_cb_t)(const int16_t frame[AUDIO_FRAME_SAMPLES], void *ctx);

typedef enum {
    AUDIO_TONE_NONE,
    AUDIO_TONE_DIAL,                /* 350+440 Hz continuous (NA) */
    AUDIO_TONE_RINGBACK,            /* 440+480 Hz, 2 s on / 4 s off */
    AUDIO_TONE_BUSY,                /* 480+620 Hz, 0.5/0.5 */
    AUDIO_TONE_RING,                /* local ringer pattern */
    AUDIO_TONE_KEYCLICK,            /* short, one-shot */
} audio_tone_t;

/** Configure I2S from board_cfg(); amp off until audio_start(). */
esp_err_t audio_init(void);

/** Start streaming. Either callback may be NULL (silence / discard). */
esp_err_t audio_start(audio_source_cb_t src, audio_sink_cb_t sink, void *ctx);
esp_err_t audio_stop(void);

/** Tone mixed on top of the source (or alone when stopped → auto-start). */
void audio_play_tone(audio_tone_t tone);
/** One-shot DTMF tone feedback for local keypress. */
void audio_play_dtmf(char digit);

void audio_set_volume(uint8_t volume);  /* 0-100, software gain */
void audio_set_mic_mute(bool mute);
