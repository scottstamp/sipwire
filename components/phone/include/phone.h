#pragma once

/* Phone: ties sip (UA) + rtp + audio together and owns the call state.
 *
 * Tasks (created by phone_init):
 *   "sip"    core 0: owns the SIP UA exclusively. Every call into this API
 *            is turned into a queued command or a short mutex-protected
 *            update, so all functions below are thread-safe and non-blocking
 *            (the LVGL task, the console and event handlers may call them).
 *   "rtp_rx" core 1: receives RTP for the current call.
 *   The audio component's I2S tasks (core 1) call the phone's source/sink
 *   callbacks every 20 ms while a call has media.
 *
 * Events (default loop): SIPWIRE_SIP_EVENT / SIP_EVENT_REG_STATE and
 * SIP_EVENT_MWI, SIPWIRE_CALL_EVENT / CALL_EVENT_STATE and SIPWIRE_BLF_EVENT /
 * BLF_EVENT_STATE (see sipwire_events.h).
 *
 * BLF and MWI: the UA subscribes to every extension of the `blf` setting
 * ("dialog" package) and to our own mailbox ("message-summary") as soon as it
 * is registered; Asterisk's unsolicited MWI NOTIFYs count as well. A changed
 * `blf` list is applied live (removed extensions are unsubscribed, new ones
 * subscribed, kept ones keep their state). States are UNKNOWN until the
 * first NOTIFY and again whenever the UA is rebuilt or a subscription fails.
 *
 * ENDED → IDLE: the phone owns this timer. After a call ends the state stays
 * CALL_ENDED for 2 s (3 s while the busy tone plays after a 486/600/603 to
 * our call), then CALL_IDLE is posted. phone_hangup() in ENDED, a new dial or
 * an incoming call skip the rest of the wait. UIs just mirror the state.
 *
 * Two call lines (see CALL_EVENT_SECOND in sipwire_events.h): the top-level
 * call fields below describe the foreground call (the one with audio;
 * `line` 1 or 2 stays with a call across swaps), `second` the background
 * call: a waiting incoming call (RINGING_IN, call-waiting beep in the
 * foreground call; setting `call_waiting`) or a held call. Only the
 * foreground call has media. When the foreground call ends the background
 * call moves to the foreground at once (a held call stays held until
 * resumed, a waiting call rings normally); its ENDED is still posted.
 *
 * Do not disturb (setting `dnd`): every incoming call gets 486 without
 * ringing and is reported as CALL_EVENT_MISSED (so is a call refused as
 * busy). Auto answer (setting `auto_answer`): an incoming call asking for it
 * (Call-Info answer-after / Alert-Info auto answer, e.g. Asterisk Page())
 * is answered on the loudspeaker without ringing when no other call
 * exists; during a call it is a normal waiting call; under DND it is refused
 * like any other call (DND wins).
 *
 * Quiet hours (settings `quiet_hours`, `quiet_start` / `quiet_end` local time, wrapping
 * past midnight, `quiet_allow_contacts`): while the clock is set (SNTP) and inside the
 * window, incoming calls are refused with 486 like DND (logged as DND, CALL_EVENT_MISSED
 * with dnd + quiet), except callers found in the contacts (when allowed) and find the
 * handset calls.
 *
 * Find the handset (setting `find_alert`, default "find"): an INVITE whose Alert-Info
 * carries the marker rings at full volume with its own pattern (and wakes the
 * screen), even with DND on; never auto-answered. During a call it waits like any
 * other call (refused as busy without a free line).
 *
 * Transfers report through CALL_EVENT_TRANSFER and `xfer`. Blind: REFER of
 * the foreground call; on success (2xx sipfrag) the call is hung up, on
 * failure it is kept (resumed if it was on hold). Attended: start holds the
 * foreground call (it becomes the background call) and dials the target;
 * complete sends REFER with Replaces to the original party and hangs up both
 * calls on success; cancel hangs up the consultation call (the original
 * stays held in the foreground).
 *
 * DTMF: RFC 4733 when the peer offered telephone-event, SIP INFO
 * (application/dtmf-relay) otherwise.
 *
 * The SIP account comes from settings (sip_server, sip_user...). The UA is
 * (re)created on every NET_EVENT_GOT_IP and when SIP settings change; with
 * an empty server or user the phone stays unregistered. */

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "rtp.h"
#include "sipwire_blf.h"
#include "sipwire_events.h"

typedef struct {
    /* registration */
    sip_reg_state_t reg;
    int reg_code;                   /* last REGISTER status, 0 = none / timeout */
    char server[64];                /* "host:port" of the active account, "" if none */
    char local_addr[24];            /* "ip:port" the UA uses, "" if none */

    /* call (the foreground call) */
    call_state_t call;
    bool outgoing;
    char number[64];
    char name[64];
    int end_code;                   /* valid in CALL_ENDED; 0 = normal hangup */
    int64_t call_start_us;          /* esp_timer time the call was answered, 0 = not yet */
    uint8_t line;                   /* line of the foreground call: 1 or 2, 0 = none */
    bool auto_answered;             /* answered automatically (page / intercom) */
    bool find;                      /* ringing as find-the-handset (Alert-Info find_alert) */

    /* background call: waiting (CALL_RINGING_IN) or on hold (CALL_HELD,
     * CALL_ACTIVE until the hold is confirmed); call == CALL_IDLE: none */
    struct {
        call_state_t call;
        bool outgoing;
        char number[64];
        char name[64];
        int64_t call_start_us;
        uint8_t line;
    } second;

    /* transfer in progress or last result (reset when the phone goes idle) */
    struct {
        xfer_state_t state;
        bool attended;
        int code;
        char target[64];
    } xfer;

    /* call features (settings) */
    bool dnd;
    bool call_waiting;
    bool auto_answer;

    /* audio */
    bool muted;
    bool speaker;
    uint8_t volume;                 /* current output volume 0-100 */

    /* media (current call, or the last one if none is active) */
    bool media_active;
    uint8_t payload_type;           /* 9 = G.722, 0 = PCMU, 8 = PCMA */
    char rtp_remote[24];            /* "ip:port" we send to */
    uint16_t rtp_local_port;
    rtp_stats_t rtp;
    uint32_t rtp_tx_errors;         /* sendto failures */

    /* BLF keys (settings `blf`, in order) and message waiting */
    uint8_t blf_count;
    struct {
        char ext[SIPWIRE_BLF_EXT_MAX];
        char label[SIPWIRE_BLF_LABEL_MAX];
        blf_state_t state;
    } blf[SIPWIRE_BLF_MAX];
    sip_event_mwi_t mwi;
} phone_status_t;

/** Start the sip and rtp_rx tasks, register event handlers and the console
 *  commands (call, answer, hangup, reject, dtmf, hold, resume, mute, unmute,
 *  vol, sip, rtp, blf, mwi, answer2, reject2, swap, xfer, axfer, dnd). Call
 *  after settings_init, net_init and audio_init. */
esp_err_t phone_init(void);

/** Dial a number / user part or full "sip:" URI. ESP_ERR_INVALID_ARG for an
 *  empty or too long number, ESP_ERR_TIMEOUT if the command queue is full.
 *  The outcome is reported through CALL_EVENT_STATE (and the log). */
esp_err_t phone_dial(const char *number);

/** Answer the ringing incoming call. */
esp_err_t phone_answer(void);

/** End the foreground call in whatever state (cancel, reject with 486 or
 *  BYE); in ENDED return to IDLE immediately. A background call moves to
 *  the foreground (still held / still ringing). */
esp_err_t phone_hangup(void);

/** Answer the waiting call: hold the foreground call (it becomes the
 *  background call), answer the waiting one in the foreground. */
esp_err_t phone_answer_waiting(void);

/** Refuse the waiting call (486). */
esp_err_t phone_reject_waiting(void);

/** Swap the two calls: hold the foreground call, resume the background one
 *  (a waiting call is answered, as phone_answer_waiting). */
esp_err_t phone_swap(void);

/** Blind transfer of the foreground call (established) to `target` (number
 *  or sip: URI). Progress: CALL_EVENT_TRANSFER. */
esp_err_t phone_transfer_blind(const char *target);

/** Attended transfer, step 1: hold the foreground call and call `target`
 *  (consultation call). Needs one established call and no second one. */
esp_err_t phone_transfer_attended_start(const char *target);

/** Step 2: connect the original party with the consultation call's party
 *  (REFER with Replaces); both calls are hung up on success. The
 *  consultation call must be answered. */
esp_err_t phone_transfer_attended_complete(void);

/** Abort: hang up the consultation call; the original stays on hold. */
esp_err_t phone_transfer_attended_cancel(void);

/** Do not disturb on/off (persists setting `dnd`). */
esp_err_t phone_set_dnd(bool on);

/** Reject the ringing incoming call with `code` (4xx-6xx; 0 → 603 Decline). */
esp_err_t phone_reject(int code);

/** Send one DTMF digit (RFC 4733) in an active call, with local feedback tone. */
esp_err_t phone_dtmf(char digit);

/** Send a digit sequence, 150 ms apart. ESP_ERR_INVALID_ARG on bad digits. */
esp_err_t phone_dtmf_string(const char *digits);

/** Hold (true) / resume (false) the active call via re-INVITE. */
esp_err_t phone_hold(bool hold);

/** Microphone mute; reset to off when the call ends. */
void phone_set_mute(bool muted);

/** Loudspeaker: switches the in-call output between settings.call_volume and
 *  a louder preset; reset to handset when the call ends. */
void phone_set_speaker(bool on);

/** Change the current output volume (ring volume when idle/ringing, call or
 *  speaker volume in a call). Ring and call volumes are persisted to settings
 *  (debounced, ~1 s after the last change). */
void phone_set_volume(uint8_t volume);

/** Consistent snapshot of the phone state. */
void phone_get_status(phone_status_t *out);

/** Quiet hours window: `now_min` (local minutes after midnight, 0-1439; < 0 = clock not
 *  set) inside [start_min, end_min). end <= start wraps past midnight (22:00-07:00);
 *  start == end is an empty window. Pure (also used by the UI bridge). */
bool phone_quiet_window(uint16_t start_min, uint16_t end_min, int now_min);
