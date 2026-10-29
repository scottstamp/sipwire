#pragma once

/* Call-control policy of the phone component, as small pure functions (no
 * FreeRTOS / IDF headers) so they can be unit-tested on the host
 * (test_host/phone). phone.c applies them. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sip_ua.h"
#include "sipwire_blf.h"
#include "sipwire_events.h"

/* ---- tones -------------------------------------------------------------- */

typedef enum {
    PHONE_TONE_NONE,
    PHONE_TONE_RINGBACK,
    PHONE_TONE_RING,
    PHONE_TONE_BUSY,
} phone_tone_t;

/** Tone to play locally in `state`. `early_media`: a 183 delivered media for
 *  this call (then the RTP stream is played instead of the ringback tone).
 *  ENDED plays busy for outgoing calls refused as busy (see phone_end_is_busy). */
phone_tone_t phone_tone_for(sip_ua_call_state_t state, bool outgoing, bool early_media, int end_code);

/* ---- call end ------------------------------------------------------------ */

#define PHONE_ENDED_HOLD_MS     2000u   /* ENDED shown this long, then IDLE */
#define PHONE_BUSY_HOLD_MS      3000u   /* ... or this long while the busy tone plays */

/** SIP status reported as end_code: 0 for a normal hangup (either side),
 *  otherwise the final status (486, 603, 404...), 487 for a cancelled
 *  incoming call, 408 for a timeout without status. */
int phone_end_code(sip_ua_end_reason_t reason, int status_code);

/** Outgoing call refused with 486 Busy Here, 600 Busy Everywhere or 603 Decline. */
bool phone_end_is_busy(bool outgoing, int end_code);

/** How long ENDED stays visible before the phone returns to IDLE. */
uint32_t phone_ended_hold_ms(bool outgoing, int end_code);

/* ---- media ---------------------------------------------------------------- */

/** What to do with the RTP stream in `state` given the negotiated direction
 *  (from our point of view). send: mic → RTP; play: RTP → speaker.
 *  Only an answered, not locally held call sends; early media is played but
 *  not sent (no audio leaves the handset before the callee answers). */
void phone_media_flags(sip_ua_call_state_t state, sip_media_dir_t dir, bool early,
                       bool *send, bool *play);

/** Symmetric RTP: accept a datagram only if it looks like RTP v2 and comes
 *  from the IP negotiated in SDP or from the SIP server's IP (any port; host
 *  byte order, 0 = none). The server IP covers a PBX that advertises a
 *  private c= address but sends from the address we register with. */
bool phone_rtp_source_ok(uint32_t sdp_ip, uint32_t server_ip, uint32_t src_ip, const uint8_t *pkt, size_t len);

/* ---- codecs ------------------------------------------------------------------ */

#define PHONE_CODEC_PREF_DEFAULT "g722,pcmu,pcma"

/** "G722", "PCMU", "PCMA" for PT 9/0/8, "?" otherwise. */
const char *phone_codec_name(uint8_t pt);

/** Parse a codec preference list such as "g722,pcmu,pcma" into static
 *  payload types for sip_ua_config_t.codecs. Names (case-insensitive):
 *  g722, pcmu or ulaw, pcma or alaw; separated by ',' and/or spaces.
 *  Returns the number of entries (1..max), or 0 if the string is empty or
 *  has an unknown name, a duplicate, or more than max entries. */
int phone_codec_pref_parse(const char *s, uint8_t *pts, int max);

/* ---- RTP port allocation --------------------------------------------------- */

#define PHONE_RTP_PORT_MIN      16384u
#define PHONE_RTP_PORT_MAX      16482u  /* inclusive, even ports only */
#define PHONE_RTP_PORT_COUNT    ((PHONE_RTP_PORT_MAX - PHONE_RTP_PORT_MIN) / 2u + 1u)

typedef struct {
    uint16_t idx;                       /* next index in [0, PHONE_RTP_PORT_COUNT) */
} phone_port_alloc_t;

/** Start at a random position so consecutive boots don't reuse the same port. */
void phone_port_alloc_init(phone_port_alloc_t *a, uint32_t seed);

/** Next even port in [MIN, MAX], round robin. Never returns `skip` (the port
 *  still in use, 0 = none) unless it is the only one. */
uint16_t phone_port_alloc_next(phone_port_alloc_t *a, uint16_t skip);

/* ---- DTMF sequencing -------------------------------------------------------- */

#define PHONE_DTMF_GAP_MS       150u    /* start-to-start spacing of queued digits */
#define PHONE_DTMF_DURATION_MS  80u     /* RFC 4733 event length (+3 end packets) */
#define PHONE_DTMF_QUEUE        32

typedef struct {
    char q[PHONE_DTMF_QUEUE];
    uint8_t head, len;
    uint64_t next_ms;                   /* earliest start of the next digit */
} phone_dtmf_seq_t;

/** '0'-'9', '*', '#', 'A'-'D' (lower case accepted). */
bool phone_dtmf_valid(char c);

void phone_dtmf_seq_clear(phone_dtmf_seq_t *s);

/** Queue all digits of `digits` (upper-cased). Rejects the whole string
 *  (returns false, queues nothing) if it is empty, holds an invalid character
 *  or does not fit. */
bool phone_dtmf_seq_push(phone_dtmf_seq_t *s, const char *digits);

/** Digit due at now_ms (not removed), or 0 if none is due yet. */
char phone_dtmf_seq_due(const phone_dtmf_seq_t *s, uint64_t now_ms);

/** Remove the due digit after it was sent; the next one is due GAP later. */
void phone_dtmf_seq_consume(phone_dtmf_seq_t *s, uint64_t now_ms);

/* ---- volume ----------------------------------------------------------------- */

typedef enum {
    PHONE_VOL_RING,                     /* settings.ring_volume */
    PHONE_VOL_CALL,                     /* settings.call_volume (handset) */
    PHONE_VOL_SPEAKER,                  /* loudspeaker preset (RAM only) */
} phone_vol_kind_t;

/** Which volume is "current" (applied to the output, changed by +/-).
 *  Idle and ringing → ring volume; any call state → call or speaker volume. */
phone_vol_kind_t phone_volume_kind(sip_ua_call_state_t state, bool speaker);

/* ---- BLF ---------------------------------------------------------------------- */

/** Event-level state for a UA BLF state. */
blf_state_t phone_blf_map(sip_blf_state_t st);

/** The `blf` list changed from `old` to `nw`: keep[i] is the index in `old`
 *  of the entry with the same extension as nw[i] (its subscription and state
 *  carry over, only the label may differ), or -1 (subscribe anew). Returns a
 *  bit mask of the old indices to unsubscribe (extension no longer listed). */
uint32_t phone_blf_plan(const sipwire_blf_entry_t *old, int n_old, const sipwire_blf_entry_t *nw, int n_new,
                        int keep[SIPWIRE_BLF_MAX]);

/** Short name for logs and the console: "unknown", "idle", "ringing", "busy". */
const char *phone_blf_state_name(blf_state_t st);
