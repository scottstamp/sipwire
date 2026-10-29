#pragma once

/* Test fixture: UA + fake transport + virtual clock + event recorder. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sip_msg.h"
#include "sip_ua.h"

#define FX_MAX_SENT   256
#define FX_MAX_EVENTS 128

#define FX_SERVER_IP  0x0a000001u  /* 10.0.0.1 */
#define FX_LOCAL_IP   0x0a000002u  /* 10.0.0.2 */

typedef struct {
    char buf[SIP_MAX_MSG_OUT + 1];
    size_t len;
    sip_addr_t to;
} fx_sent_t;

/* FXE_BLF: call = subscription handle, state = sip_blf_state_t, number = ext.
 * FXE_MWI: state = waiting, reason = new, code = old. */
/* FXE_XFER: state = sip_xfer_state_t, code. FXE_REJECTED: number, display, code.
 * FXE_CALL: answer_after as reported. */
typedef enum { FXE_REG, FXE_INCOMING, FXE_CALL, FXE_MEDIA, FXE_BLF, FXE_MWI, FXE_XFER, FXE_REJECTED } fx_ev_kind_t;

typedef struct {
    fx_ev_kind_t kind;
    sip_call_handle_t call;
    int state;
    int reason;
    int code;
    int answer_after;
    char number[64];
    char display[64];
    sip_media_info_t media;
} fx_ev_t;

typedef struct fx {
    sip_ua_t *ua;
    uint64_t now;
    uint32_t rng;
    fx_sent_t sent[FX_MAX_SENT];
    int n_sent;
    fx_ev_t ev[FX_MAX_EVENTS];
    int n_ev;
    /* behaviour hooks */
    bool auto_answer;
    bool hangup_in_callback;
    bool destroy_in_callback;
    bool auto_reg;          /* fx_advance answers new REGISTERs with 200 */
    int reg_scan;
    sip_addr_t server;
} fx_t;

/* Default config: server 10.0.0.1:5060, local 10.0.0.2:5060, user 1001,
 * password "secret", display "Alice", RTP 4000. cfg_tweak may be NULL. */
void fx_init(fx_t *f, void (*cfg_tweak)(sip_ua_config_t *cfg));
void fx_free(fx_t *f);

/* Advance the virtual clock in 10 ms ticks. */
void fx_advance(fx_t *f, uint64_t ms);

/* Index of the n-th most recent sent message starting with prefix ("INVITE",
 * "SIP/2.0 180"...), counting from the end (nth = 0 → latest). -1 if none. */
int fx_find(const fx_t *f, const char *prefix, int nth);
int fx_count(const fx_t *f, const char *prefix);
void fx_clear_sent(fx_t *f);

/* Parse sent[idx] (copied to a static scratch, valid until the next call). */
const sip_msg_t *fx_parse(const fx_t *f, int idx);

/* Header value (first match, by full name) of sent[idx] in a static buffer. */
const char *fx_hdr(const fx_t *f, int idx, const char *name);

/* Feed a message "from the server". */
void fx_inject(fx_t *f, const char *msg);

/* Reply to sent request idx: copies Via/From/To/Call-ID/CSeq, adds to_tag
 * (if not NULL and To has none), extra headers (CRLF-terminated) and an SDP
 * body if not NULL. */
void fx_reply(fx_t *f, int idx, int code, const char *to_tag, const char *extra, const char *body);

/* Events */
int fx_count_ev(const fx_t *f, fx_ev_kind_t kind);
const fx_ev_t *fx_last_ev(const fx_t *f, fx_ev_kind_t kind);

/* A typical server SDP offer/answer at 10.0.0.1:20000 */
extern const char *FX_SDP_PCMU;
extern const char *FX_SDP_G722;        /* Asterisk-style "9 0 8 101" */
extern const char *FX_SDP_PCMA_ONLY;
extern const char *FX_SDP_G729_ONLY;
extern const char *FX_SDP_SENDONLY;

/* xorshift32 */
uint32_t fx_rand(uint32_t *state);
