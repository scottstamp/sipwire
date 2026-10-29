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

/* ---- acoustic echo canceller (esp-sr AEC on the mic path) ---- */

/** Switch the canceller live (setting `aec`). The first enable allocates it
 *  (PSRAM when present); ESP_ERR_NOT_SUPPORTED when built without
 *  CONFIG_SIPWIRE_AEC. Off = the mic path skips it entirely. */
esp_err_t audio_set_aec(bool on);
/** Reference delay in samples (speaker index -> mic index), or -1 for the
 *  built-in default (the TX DMA depth). For tuning from the console. */
esp_err_t audio_set_aec_delay(int samples);
/** esp-sr residual echo suppression level, 0 normal .. 2 very aggressive. */
esp_err_t audio_set_aec_nlp(int level);

typedef struct {
    bool built;                     /* CONFIG_SIPWIRE_AEC */
    bool enabled;                   /* setting / audio_set_aec */
    bool ready;                     /* instance allocated */
    bool running;                   /* processing the current stream */
    const char *mode;               /* esp-sr mode name */
    bool bypassed;                  /* QEMU build: esp-sr not called (pass-through) */
    int nlp_level;
    uint32_t block_samples;         /* esp-sr frame size */
    uint32_t latency_samples;       /* added by adapting 20 ms frames to blocks */
    uint32_t delay_samples;         /* reference delay in use (incl. xrun shifts) */
    bool delay_override;
    uint32_t mem_internal, mem_psram;   /* heap taken by the instance + buffers */
    /* since the canceller (re)started in the current / last stream */
    uint32_t frames;                /* 20 ms mic frames processed */
    uint32_t blocks;                /* esp-sr blocks processed */
    uint32_t avg_us;                /* processing time per 20 ms frame */
    uint32_t max_us;                /* worst single block */
    uint32_t ref_late;              /* reference samples not yet written (delay too small) */
    uint32_t tx_xruns, rx_xruns;    /* I2S queue overflows (each shifts the delay by 10 ms) */
    /* echo estimates over blocks with far-end signal: dB, 0 = no data */
    float erl_db;                   /* speaker reference -> mic (coupling) */
    float erle_db;                  /* mic -> canceller output, whole run */
    float erle_recent_db;           /* same, last ~2 s with far-end signal */
} audio_aec_stats_t;

void audio_get_aec_stats(audio_aec_stats_t *out);
