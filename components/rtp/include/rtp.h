#pragma once

/* RTP session for one call: RFC 3550 packetization, RFC 4733 DTMF send,
 * adaptive jitter buffer. Pure C: no IDF headers. The app owns the socket
 * and the clock; this module only builds/parses packets and buffers audio.
 * Not thread-safe: callers serialize access (one mutex around the session).
 *
 * PCM at this API is always 16 kHz, 20 ms = RTP_FRAME_SAMPLES samples,
 * whatever the codec:
 *   PT 9 G.722  wideband, encoded directly from / decoded directly to 16 kHz;
 *   PT 0 PCMU, PT 8 PCMA  narrowband: decimated 16 -> 8 kHz before encoding
 *               and interpolated 8 -> 16 kHz after decoding (half-band FIR).
 * On the wire all three run at 64 kbit/s with an 8000 Hz RTP clock: G.722's
 * clock rate is 8000 although it samples at 16 kHz (RFC 3551 4.5.2), so
 * every 20 ms packet carries RTP_FRAME_BYTES of payload and advances the
 * timestamp by RTP_FRAME_TS. telephone-event uses the same 8000 Hz clock. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RTP_FRAME_MS        20
#define RTP_PCM_RATE        16000   /* PCM at the API, every codec */
#define RTP_FRAME_SAMPLES   320     /* 20 ms @ 16 kHz */
#define RTP_FRAME_TS        160     /* timestamp units per 20 ms (8000 Hz RTP clock) */
#define RTP_FRAME_BYTES     160     /* payload bytes per 20 ms (64 kbit/s) */
#define RTP_MAX_PACKET      (12 + 2 * RTP_FRAME_BYTES)

#define RTP_PT_PCMU         0
#define RTP_PT_PCMA         8
#define RTP_PT_G722         9

typedef struct rtp_session rtp_session_t;

typedef struct {
    uint8_t payload_type;           /* RTP_PT_PCMU, RTP_PT_PCMA or RTP_PT_G722 */
    uint8_t dtmf_payload_type;      /* telephone-event PT, 0 = disabled */
    uint32_t ssrc;                  /* random, from the app */
    uint16_t initial_seq;           /* random */
    uint32_t initial_ts;            /* random */
    /* jitter buffer, in ms; adapts between min and max */
    uint16_t jb_min_ms;             /* default 40 */
    uint16_t jb_max_ms;             /* default 160 */
} rtp_config_t;

typedef struct {
    uint32_t rx_packets, rx_lost, rx_late, rx_dup, rx_invalid;
    uint32_t tx_packets;
    uint32_t plc_frames;            /* frames concealed */
    uint16_t jb_target_ms, jb_depth_ms;
    uint32_t jitter_ms;             /* RFC 3550 interarrival jitter */
} rtp_stats_t;

rtp_session_t *rtp_session_create(const rtp_config_t *cfg);
void rtp_session_destroy(rtp_session_t *s);

/** Encode one 20 ms frame of 16 kHz PCM and build a packet into `out`; while
 *  a DTMF event is in progress this builds the event packet instead (PCM is
 *  not sent). Call every 20 ms. Returns packet length, 0 on bad args/out_cap. */
size_t rtp_build_audio(rtp_session_t *s, const int16_t pcm[RTP_FRAME_SAMPLES],
                       uint8_t *out, size_t out_cap);

/** Start an RFC 4733 event (digit '0'-'9','*','#','A'-'D'). Subsequent
 *  rtp_build_audio() calls emit event packets instead of audio for
 *  `duration_ms`, then 3 end packets. Returns false if busy or disabled. */
bool rtp_send_dtmf(rtp_session_t *s, char digit, uint16_t duration_ms);

/** Feed a received datagram. `now_ms` is a monotonic clock. Packets with a
 *  different PT/SSRC switch or are dropped per RFC 3550 rules. */
void rtp_on_packet(rtp_session_t *s, const uint8_t *pkt, size_t len, uint32_t now_ms);

/** Pull exactly one 20 ms frame of decoded 16 kHz PCM for playout. Always
 *  fills the frame: real audio, concealment (PLC), or silence before the
 *  first packet. */
void rtp_pull_frame(rtp_session_t *s, int16_t pcm[RTP_FRAME_SAMPLES], uint32_t now_ms);

void rtp_get_stats(const rtp_session_t *s, rtp_stats_t *out);
