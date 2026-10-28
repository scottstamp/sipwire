#pragma once

/* RTP session for one call: RFC 3550 packetization, RFC 4733 DTMF send,
 * adaptive jitter buffer. Pure C: no IDF headers. The app owns the socket
 * and the clock; this module only builds/parses packets and buffers audio.
 * Not thread-safe: callers serialize access (one mutex around the session). */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RTP_FRAME_MS        20
#define RTP_SAMPLES_8K      160     /* 20 ms @ 8 kHz */
#define RTP_MAX_PACKET      (12 + 320)

typedef struct rtp_session rtp_session_t;

typedef struct {
    uint8_t payload_type;           /* 0 = PCMU, 8 = PCMA */
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

/** Encode one 20 ms frame of 8 kHz PCM and build a packet into `out`.
 *  Returns packet length, 0 if suppressed (e.g. DTMF in progress). */
size_t rtp_build_audio(rtp_session_t *s, const int16_t pcm[RTP_SAMPLES_8K],
                       uint8_t *out, size_t out_cap);

/** Start an RFC 4733 event (digit '0'-'9','*','#','A'-'D'). Subsequent
 *  rtp_build_audio() calls emit event packets instead of audio for
 *  `duration_ms`, then 3 end packets. Returns false if busy or disabled. */
bool rtp_send_dtmf(rtp_session_t *s, char digit, uint16_t duration_ms);

/** Feed a received datagram. `now_ms` is a monotonic clock. Packets with a
 *  different PT/SSRC switch or are dropped per RFC 3550 rules. */
void rtp_on_packet(rtp_session_t *s, const uint8_t *pkt, size_t len, uint32_t now_ms);

/** Pull exactly one 20 ms frame of decoded PCM for playout. Always fills the
 *  frame: real audio, concealment (PLC), or silence before first packet. */
void rtp_pull_frame(rtp_session_t *s, int16_t pcm[RTP_SAMPLES_8K], uint32_t now_ms);

void rtp_get_stats(const rtp_session_t *s, rtp_stats_t *out);
