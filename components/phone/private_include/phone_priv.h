#pragma once

/* Internal interface between phone.c (sip task, call state) and
 * phone_media.c (RTP socket, rtp_rx task, audio callbacks). */

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "phone.h"
#include "phone_logic.h"
#include "sip_ua.h"

/** Create the media mutex and the rtp_rx task. */
esp_err_t phone_media_init(void);

/** UA rtp_port_alloc hook (sip task): bind a socket on the next free even
 *  port and keep it for the coming media callback. */
uint16_t phone_media_alloc_port(void *ctx);

/** IP of the SIP server (host order), accepted as an RTP source too. */
void phone_media_set_server_ip(uint32_t ip);

/** Negotiated media appeared or changed (sip task). */
void phone_media_update(const sip_media_info_t *mi);

/** Call state changed: recompute send/play (sip task). */
void phone_media_set_state(sip_ua_call_state_t state);

/** Call over: free the RTP session and socket (sip task). Stats are kept. */
void phone_media_teardown(void);

/** An RTP session exists (media was negotiated for the current call). */
bool phone_media_active(void);

/** The current media came from a 183 (early media, call not answered yet). */
bool phone_media_early(void);

/** Install / remove the audio source+sink callbacks and the local tone
 *  (sip task). Starts or stops audio streaming as needed. */
void phone_media_audio(bool stream, phone_tone_t tone);

typedef enum {
    PHONE_DTMF_SENT,
    PHONE_DTMF_BUSY,            /* previous event still being sent: retry */
    PHONE_DTMF_UNAVAILABLE,     /* no session / not sending / no telephone-event */
} phone_dtmf_res_t;

/** Start one RFC 4733 event (sip task). */
phone_dtmf_res_t phone_media_send_dtmf(char digit);

/** Fill the media part of a status snapshot. */
void phone_media_status(phone_status_t *st);

/** Register the call-control console commands (phone_console.c). */
esp_err_t phone_console_register(void);
