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

    /* call */
    call_state_t call;
    bool outgoing;
    char number[64];
    char name[64];
    int end_code;                   /* valid in CALL_ENDED; 0 = normal hangup */
    int64_t call_start_us;          /* esp_timer time the call was answered, 0 = not yet */

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
 *  vol, sip, rtp, blf, mwi). Call after settings_init, net_init and audio_init. */
esp_err_t phone_init(void);

/** Dial a number / user part or full "sip:" URI. ESP_ERR_INVALID_ARG for an
 *  empty or too long number, ESP_ERR_TIMEOUT if the command queue is full.
 *  The outcome is reported through CALL_EVENT_STATE (and the log). */
esp_err_t phone_dial(const char *number);

/** Answer the ringing incoming call. */
esp_err_t phone_answer(void);

/** End the call in whatever state (cancel, reject with 486 or BYE); in ENDED
 *  return to IDLE immediately. */
esp_err_t phone_hangup(void);

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
