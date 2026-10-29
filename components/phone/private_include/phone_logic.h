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
    PHONE_TONE_CALL_WAITING,            /* beep in the foreground call while a call waits */
} phone_tone_t;

/** Tone to play locally in `state`. `early_media`: a 183 delivered media for
 *  this call (then the RTP stream is played instead of the ringback tone).
 *  ENDED plays busy for outgoing calls refused as busy (see phone_end_is_busy). */
phone_tone_t phone_tone_for(sip_ua_call_state_t state, bool outgoing, bool early_media, int end_code);

/** Tone with a background call taken into account: the call-waiting beep
 *  while the foreground call is established (ACTIVE / HELD) and the
 *  background call rings (`bg` = its state, IDLE if none). Otherwise `base`. */
phone_tone_t phone_tone_waiting(phone_tone_t base, sip_ua_call_state_t fg, sip_ua_call_state_t bg);

/* ---- call features ---------------------------------------------------------- */

/** Answer an incoming call automatically (page / intercom)? Only when the
 *  caller asked for it (answer_after >= 0, from Call-Info / Alert-Info), the
 *  `auto_answer` setting is on and no other call exists (a page during a call
 *  is offered as a normal waiting call). DND never gets here: the UA refuses
 *  every call with 486 first (a page under DND is ignored). */
bool phone_auto_answer(int answer_after, bool setting_on, bool other_call);

/** Event-level transfer state for a UA transfer report. */
xfer_state_t phone_xfer_map(sip_xfer_state_t st);

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

/* ---- call log ------------------------------------------------------------- */

/* Same values as dir_result_t (directory.h); phone.c asserts that. */
typedef enum {
    PHONE_LOG_ANSWERED = 0,
    PHONE_LOG_MISSED = 1,
    PHONE_LOG_REJECTED = 2,
    PHONE_LOG_DND = 3,
} phone_log_result_t;

/** Result of an ended call. `answered`: media was established (the call was
 *  ACTIVE at some point; transferred calls were). `rejected_local`: the user
 *  declined the ringing incoming call. `dnd`: refused by do-not-disturb.
 *  An outgoing call that never connected is MISSED (no answer / failed). */
phone_log_result_t phone_log_result(bool outgoing, bool answered, bool rejected_local, bool dnd);

/** Talk time in whole seconds from start_us (0 = never answered) to now_us. */
uint32_t phone_log_duration_s(int64_t start_us, int64_t now_us);

/** Wall-clock start for the log: now minus the talk time (unanswered: now,
 *  the moment the call went away); 0 when the clock is not set (now_wall <= 0). */
int64_t phone_log_start(int64_t now_wall, uint32_t duration_s);

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
