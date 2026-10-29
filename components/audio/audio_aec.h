#pragma once

/* Echo canceller glue between the audio tasks and components/aec (audio_aec.c).
 * Private to the audio component; the public half is in audio.h.
 *
 * Speaker and mic share one I2S clock, so a speaker sample's index (count of
 * samples written to I2S TX) and a mic sample's index (count read from RX)
 * advance in lockstep: the echo of speaker sample i always lands at mic index
 * i + delay, with delay fixed by the TX DMA depth (plus a few samples of
 * converter + acoustic delay the adaptive filter absorbs). The TX task stores
 * what it writes in a ring by index; the RX task reads index - delay + a 10 ms
 * lead (aec_ref_offset), so an echo slightly earlier than expected is still
 * inside the filter. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/** Once, from audio_init. default_delay: speaker->mic index offset;
 *  xrun_shift: samples one I2S queue overflow shifts that offset by. */
esp_err_t audio_aec_init(uint32_t default_delay, uint32_t xrun_shift);
/** Stream start, before the audio tasks run: sample indices restart at 0. */
void audio_aec_session_start(void);
/** Stream stopped, the audio tasks have exited. */
void audio_aec_session_end(void);
/** TX task: n samples were just handed to I2S (always call, AEC on or off). */
void audio_aec_tx(const int16_t *pcm, size_t n);
/** RX task: one AUDIO_FRAME_SAMPLES mic frame, processed in place when the
 *  canceller is on and `want` (someone consumes the mic). Always call. */
void audio_aec_rx(int16_t *pcm, bool want);
/** I2S queue overflow callbacks (ISR). */
void audio_aec_xrun(bool tx);
