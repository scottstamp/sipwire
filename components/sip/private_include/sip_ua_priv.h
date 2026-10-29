#pragma once

/* Internals of the UA shared by sip_ua.c (registration, calls, dispatch) and
 * sip_sub.c (SUBSCRIBE/NOTIFY). Not part of the public API. */

#include <stdbool.h>
#include <stdint.h>

#include "sip_auth.h"
#include "sip_dialog.h"
#include "sip_event.h"
#include "sip_msg.h"
#include "sip_sdp.h"
#include "sip_txn.h"
#include "sip_ua.h"

#define MAX_CALLS        2
#define EVQ_LEN          24
#define STR_MAX          64
#define TAG_LEN          16
#define REG_BACKOFF_MIN  5000u
#define REG_BACKOFF_MAX  300000u
#define ALLOW_HDR        "Allow: INVITE, ACK, BYE, CANCEL, OPTIONS, INFO, NOTIFY\r\n"
#define UA_HDR           "User-Agent: " SIP_UA_USER_AGENT "\r\n"

typedef enum {
    OWN_NONE = 0,
    OWN_REG,
    OWN_KEEPALIVE,
    OWN_INVITE,        /* our initial INVITE */
    OWN_REINVITE,      /* our re-INVITE (hold) */
    OWN_CANCEL,
    OWN_BYE,
    OWN_SERVER_INVITE, /* incoming INVITE / re-INVITE */
    OWN_SUBSCRIBE,     /* SUBSCRIBE (initial, refresh, un-SUBSCRIBE); owner_ref = sub handle */
} owner_t;

typedef enum {
    CS_FREE = 0,
    CS_OUT_CALLING,    /* INVITE sent, nothing back yet but maybe 100 */
    CS_OUT_EARLY,      /* 18x received */
    CS_IN_RINGING,     /* incoming, 180 sent */
    CS_CONFIRMED,      /* 2xx sent or received */
    CS_ENDING,         /* ENDED reported; protocol cleanup still running */
} call_st_t;

/* What a UAS needs to answer a request after the datagram is gone. */
typedef struct {
    char hdrs[800];            /* Via..., From, Call-ID, CSeq lines */
    uint16_t hdrs_len;
    char to[SIP_NAMEADDR_MAX + 96];
    bool to_has_tag;
    char rr[512];              /* Record-Route lines */
    uint16_t rr_len;
} resp_tpl_t;

/* What our SDP said last time, to decide on an o= version bump. */
typedef struct {
    uint32_t ip;
    uint16_t port;
    uint8_t pts[SIP_UA_MAX_CODECS];
    uint8_t n_pts;
    int16_t dtmf;
    uint8_t dir;
    bool valid;
} sdp_sig_t;

typedef struct {
    call_st_t st;
    sip_call_handle_t handle;
    bool outgoing;
    bool end_reported;
    int last_code;
    char number[STR_MAX];
    char display[STR_MAX];

    sip_dialog_t dlg;
    bool dlg_valid;

    /* UAC: initial INVITE */
    char ruri[SIP_URI_MAX];
    char to_na[SIP_URI_MAX + 2];
    char call_id[48];
    char local_tag[TAG_LEN + 1];
    uint32_t invite_cseq;
    uint32_t inv_txn;
    uint8_t auth_tries;
    bool got_provisional;
    bool cancel_pending;
    bool cancel_sent;
    char ack[SIP_MAX_MSG_OUT];
    uint16_t ack_len;
    uint32_t ack_cseq;

    /* UAS */
    resp_tpl_t tpl;
    bool awaiting_ack;         /* 2xx sent, ACK not yet seen */
    uint32_t uas_ack_cseq;
    uint32_t uas_inv_txn;
    bool bye_pending;          /* hang up once the ACK arrives */
    bool offer_in_2xx;         /* we offered in the 2xx; answer comes in ACK */

    /* media */
    uint16_t rtp_port;
    uint32_t sdp_id;
    uint32_t sdp_ver;
    sdp_sig_t sdp_last;        /* parameters of the last SDP we sent */
    sip_sdp_t offer;           /* remote offer awaiting our answer */
    bool have_offer;
    sip_media_info_t media;
    bool media_valid;
    bool local_hold;

    /* our re-INVITE */
    uint32_t reinv_txn;
    bool reinv_hold;
    uint8_t reinv_auth_tries;

    uint64_t deadline;         /* CS_ENDING cleanup */
} call_t;

typedef struct {
    sip_ua_reg_state_t state;
    int code;
    bool reported;
    sip_ua_reg_state_t rep_state;
    int rep_code;
    bool active;               /* app wants to be registered */
    bool unregistering;
    char call_id[48];
    char tag[TAG_LEN + 1];
    uint32_t cseq;
    uint32_t txn;
    uint32_t sent_expires;
    uint8_t auth_tries;
    uint8_t interval_tries;
    uint32_t expires_req;
    uint32_t granted;
    uint64_t t_refresh;
    uint64_t t_retry;
    uint32_t backoff_ms;
    uint64_t t_keepalive;
} reg_t;


/* ---- subscriptions (sip_sub.c) ---- */

#define SUB_DLG_MAX      4      /* remembered dialogs of a watched extension */
#define SUB_MAX_INFLIGHT 3      /* SUBSCRIBE transactions in flight at once */

typedef enum {
    SUB_FREE = 0,
    SUB_WAIT,          /* not subscribed: waiting for registration / retry time */
    SUB_TRYING,        /* initial SUBSCRIBE sent, no 2xx yet */
    SUB_ACTIVE,        /* 2xx or NOTIFY seen: dialog established */
    SUB_ENDING,        /* un-SUBSCRIBE sent; freed on its response / timeout */
} sub_st_t;

typedef enum { SUB_KIND_BLF, SUB_KIND_MWI } sub_kind_t;

typedef struct {
    char id[32];               /* dialog id (truncated) */
    uint8_t state;             /* sip_dlg_state_t */
    uint8_t dir;               /* sip_dlg_dir_t */
} sub_dlg_t;

typedef struct {
    sub_st_t st;
    sub_kind_t kind;
    sip_sub_handle_t handle;
    bool want;                 /* the app still wants it (false: being removed) */
    char target[STR_MAX];      /* BLF: extension; MWI: our user */
    char ruri[SIP_URI_MAX];    /* also the To URI */
    char call_id[48];
    char local_tag[TAG_LEN + 1];
    uint32_t cseq;             /* last CSeq used before the dialog exists */
    sip_dialog_t dlg;
    bool dlg_valid;
    uint32_t txn;              /* SUBSCRIBE in flight */
    uint32_t txn_expires;      /* Expires of that SUBSCRIBE */
    bool txn_in_dialog;
    uint8_t auth_tries;
    uint8_t interval_tries;
    uint32_t expires_req;
    uint64_t t_refresh;        /* next refresh (ACTIVE) */
    uint64_t t_expire;         /* subscription runs out (ACTIVE) */
    uint64_t t_retry;          /* next attempt (WAIT); 0 = as soon as registered */
    uint32_t backoff_ms;
    uint64_t t_started;        /* last initial SUBSCRIBE (loop protection) */
    bool refresh_now;          /* refresh at the next tick (target refresh, version gap) */
    /* BLF state */
    int reported;              /* last reported sip_blf_state_t, -1 = none */
    bool have_version;
    uint32_t version;
    uint8_t n_dlg;
    sub_dlg_t dlg_state[SUB_DLG_MAX];
} sub_t;

typedef enum { EV_REG, EV_INCOMING, EV_CALL, EV_MEDIA, EV_BLF, EV_MWI } ev_kind_t;

typedef struct {
    ev_kind_t kind;
    sip_call_handle_t call;    /* EV_BLF: subscription handle */
    int state;                 /* EV_BLF: sip_blf_state_t; EV_MWI: waiting */
    int reason;                /* EV_MWI: new messages */
    int code;                  /* EV_MWI: old messages */
    bool outgoing;
    char number[STR_MAX];      /* EV_BLF: extension */
    char display[STR_MAX];
    sip_media_info_t media;
} ev_t;

struct sip_ua {
    /* configuration (copied) */
    char server_host[STR_MAX];
    uint16_t server_port;
    sip_addr_t server;
    char domain[STR_MAX];
    char user[STR_MAX];
    char auth_user[STR_MAX];
    char password[STR_MAX];
    char display[STR_MAX];
    uint32_t expires;
    sip_addr_t local;
    sip_addr_t contact;        /* = local unless NAT discovered */
    uint16_t rtp_port;
    sip_ua_rtp_port_fn rtp_alloc;
    void *rtp_ctx;
    uint8_t codecs[SIP_UA_MAX_CODECS];
    uint8_t n_codecs;
    int dtmf_pt;
    uint8_t ptime;
    uint32_t keepalive_s;
    sip_keepalive_mode_t ka_mode;
    bool nat_fix;
    bool any_source;
    sip_ua_send_fn send;
    void *send_ctx;
    sip_ua_rand_fn rand;
    void *rand_ctx;
    sip_ua_log_fn log;
    void *log_ctx;
    sip_log_level_t log_level;
    sip_ua_callbacks_t cb;

    /* derived */
    char from_na[SIP_NAMEADDR_MAX];  /* "Name" <sip:user@domain> */
    char registrar[SIP_URI_MAX];     /* sip:domain[:port] */
    char hostpart[STR_MAX + 8];      /* domain[:port] for URIs */

    /* state */
    uint64_t now;
    sip_txn_layer_t txl;
    reg_t reg;
    sip_auth_t auth;                 /* cached credentials */
    call_t calls[MAX_CALLS];
    sip_call_handle_t next_handle;
    sub_t subs[SIP_UA_MAX_SUBS];
    sip_sub_handle_t next_sub;
    uint32_t sub_expires;
    bool mwi_reported;               /* last MWI state, to report changes only */
    bool mwi_waiting;
    uint32_t mwi_new, mwi_old;

    ev_t evq[EVQ_LEN];
    int ev_head;
    int ev_count;
    int depth;
    bool draining;
    bool destroy_pending;

    /* scratch (kept off the stack) */
    sip_msg_t msg;
    char rx[SIP_MAX_MSG_IN + 1];
    char tx[SIP_MAX_MSG_OUT + 1];
    char sdp[700];
    resp_tpl_t tpl_scratch;
};


#define LOGE(...) sipua_log(ua, SIP_LOG_ERROR, __VA_ARGS__)
#define LOGW(...) sipua_log(ua, SIP_LOG_WARN, __VA_ARGS__)
#define LOGI(...) sipua_log(ua, SIP_LOG_INFO, __VA_ARGS__)
#define LOGD(...) sipua_log(ua, SIP_LOG_DEBUG, __VA_ARGS__)

enum {
    RF_CONTACT = 1,
    RF_RR = 2,
    RF_ALLOW = 4,
};

/* ---- helpers in sip_ua.c ---- */
void sipua_log(sip_ua_t *ua, sip_log_level_t lvl, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
void sipua_log_msg(sip_ua_t *ua, const char *dir, const char *buf, size_t len, const sip_addr_t *peer);
void sipua_send_raw(sip_ua_t *ua, const sip_addr_t *to, const char *buf, size_t len);
void sipua_new_branch(sip_ua_t *ua, char out[32]);
void sipua_new_tag(sip_ua_t *ua, char out[TAG_LEN + 1]);
void sipua_new_call_id(sip_ua_t *ua, char out[48]);
void sipua_sent_by(sip_ua_t *ua, char out[24]);
void sipua_w_via(sip_ua_t *ua, sip_wbuf_t *w, const char *branch);
void sipua_w_contact(sip_ua_t *ua, sip_wbuf_t *w);
void sipua_w_auth(sip_ua_t *ua, sip_wbuf_t *w, const char *method, const char *uri);
ev_t *sipua_ev_push(sip_ua_t *ua, ev_kind_t kind, sip_call_handle_t call);
int sipua_tpl_from_msg(resp_tpl_t *tpl, const sip_msg_t *m, const sip_addr_t *src);
void sipua_respond_tpl(sip_ua_t *ua, sip_txn_t *t, const resp_tpl_t *tpl, const sip_addr_t *peer, int code,
                       const char *to_tag, unsigned flags, const char *extra, const char *body, size_t body_len);
void sipua_respond_msg(sip_ua_t *ua, sip_txn_t *t, const sip_msg_t *m, const sip_addr_t *from, int code,
                       unsigned flags, const char *extra);
bool sipua_auth_retry(sip_ua_t *ua, uint8_t *tries, const sip_msg_t *m);
uint32_t sipua_refresh_delay_s(uint32_t expires);
/* "sip:" URI for a number / user part (escaped) or a full sip: URI. */
int sipua_build_ruri(sip_ua_t *ua, const char *number, char *out, size_t cap);
void sipua_enter(sip_ua_t *ua);
void sipua_leave(sip_ua_t *ua);

/* ---- sip_sub.c ---- */
void sipua_sub_init(sip_ua_t *ua);
void sipua_sub_tick(sip_ua_t *ua);
void sipua_sub_on_response(sip_ua_t *ua, sip_txn_t *t, const sip_msg_t *m);
void sipua_sub_on_timeout(sip_ua_t *ua, sip_txn_t *t);
/* NOTIFY (in or out of dialog). Returns false if it matched no subscription
 * and is not an unsolicited message-summary (caller answers 481). */
bool sipua_sub_on_notify(sip_ua_t *ua, sip_txn_t *t, const sip_msg_t *m, const sip_addr_t *from);
/* registration reached REGISTERED (start pending subscriptions now) */
void sipua_sub_registered(sip_ua_t *ua);
/* our Contact changed: refresh established subscriptions */
void sipua_sub_contact_changed(sip_ua_t *ua);
/* un-SUBSCRIBE everything (sip_ua_unregister) */
void sipua_sub_terminate_all(sip_ua_t *ua);
