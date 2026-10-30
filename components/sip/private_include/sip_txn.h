#pragma once

/* RFC 3261 §17 transactions over UDP, plus the RFC 6026 "Accepted" state for
 * INVITE 2xx handling:
 *   client INVITE     Calling → Proceeding → Completed (ACK, timer D) |
 *                     Accepted (2xx passed up, timer M); Proceeding is
 *                     bounded by timer C (SIP_TIMER_C_MS, restarted by each
 *                     1xx) and, once CANCELled, by 64*T1 (§9.1)
 *   client non-INVITE Trying → Proceeding → Completed (timer K)
 *   server INVITE     Proceeding → Completed (G/H) → Confirmed (I) |
 *                     Accepted (2xx retransmitted until the TU sees the ACK,
 *                     §13.3.1.4; 64*T1 without ACK → TU timeout)
 *   server non-INVITE Trying → Proceeding → Completed (timer J)
 * Server transactions are also armed with a safety timeout at creation
 * (64*T1 non-INVITE, SIP_TIMER_C_MS INVITE): a request the TU never answers
 * with a final response (e.g. the response did not fit) must not hold its
 * pool slot forever while absorbing retransmissions.
 * The layer owns a fixed pool. The transaction user (UA) refers to entries
 * by id, so a freed slot is never confused with a reused one. */

#include <stdbool.h>
#include <stdint.h>

#include "sip_msg.h"
#include "sip_ua.h"

#define SIP_T1_MS          500u
#define SIP_T2_MS          4000u
#define SIP_T4_MS          5000u
#define SIP_TIMER_D_MS     32000u
/* Longest an INVITE may stay without a final response (RFC 3261 timer C is
 * "> 3 min"): our client INVITE after a provisional, and a server INVITE
 * still ringing (the TU then answers 480). */
#define SIP_TIMER_C_MS     180000u
#define SIP_MAX_TXN        16
#define SIP_BRANCH_MAX     72
#define SIP_SENTBY_MAX     80

typedef enum {
    SIP_TXN_FREE = 0,
    SIP_TXN_CLIENT_INVITE,
    SIP_TXN_CLIENT_NONINVITE,
    SIP_TXN_SERVER_INVITE,
    SIP_TXN_SERVER_NONINVITE,
} sip_txn_kind_t;

typedef enum {
    SIP_TS_CALLING,
    SIP_TS_TRYING,
    SIP_TS_PROCEEDING,
    SIP_TS_COMPLETED,
    SIP_TS_CONFIRMED,
    SIP_TS_ACCEPTED,
} sip_txn_state_t;

typedef struct {
    uint32_t id;                    /* 0 = free */
    sip_txn_kind_t kind;
    sip_txn_state_t state;
    sip_method_t method;
    char branch[SIP_BRANCH_MAX];
    char sent_by[SIP_SENTBY_MAX];   /* server: top Via "host:port" */
    uint32_t cseq;
    sip_addr_t peer;
    uint64_t t_retx;                /* next retransmission, 0 = none */
    uint32_t retx_iv;
    uint64_t t_end;                 /* timeout / linger end, 0 = none */
    int last_status;
    bool cancelled;                 /* client INVITE: CANCEL sent (§9.1 64*T1 bound) */
    int owner;                      /* TU bookkeeping */
    int owner_ref;
    uint16_t len;
    char buf[SIP_MAX_MSG_OUT];      /* client: request (then ACK); server: last response */
} sip_txn_t;

typedef struct {
    void (*send)(void *ctx, const sip_addr_t *to, const char *buf, size_t len);
    /* client: provisional/final response to pass up (incl. 2xx in Accepted) */
    void (*on_response)(void *ctx, sip_txn_t *t, const sip_msg_t *m);
    /* client: timer B/F, INVITE timer C / CANCEL bound; server INVITE: timer H, no ACK for 2xx, or no final
     * response within SIP_TIMER_C_MS (still Proceeding). A TU that responds
     * on the transaction from this callback keeps it alive (new timers). */
    void (*on_timeout)(void *ctx, sip_txn_t *t);
    void *ctx;
} sip_txn_tu_t;

typedef struct {
    sip_txn_t pool[SIP_MAX_TXN];
    uint32_t next_id;
    sip_txn_tu_t tu;
    char scratch[SIP_MAX_MSG_OUT];  /* ACK construction (keeps stack use low) */
} sip_txn_layer_t;

void sip_txn_layer_init(sip_txn_layer_t *l, const sip_txn_tu_t *tu);
sip_txn_t *sip_txn_find(sip_txn_layer_t *l, uint32_t id);
int sip_txn_count(const sip_txn_layer_t *l);
void sip_txn_tick(sip_txn_layer_t *l, uint64_t now);
/* Earliest armed retransmission / timeout of any transaction, UINT64_MAX if none. */
uint64_t sip_txn_next_deadline(const sip_txn_layer_t *l);
void sip_txn_terminate(sip_txn_t *t);

/* Client: copy + send the request and arm timers. NULL if pool full or the
 * request does not fit. */
sip_txn_t *sip_txn_client_start(sip_txn_layer_t *l, uint64_t now, sip_method_t method,
                                const char *branch, uint32_t cseq, const sip_addr_t *peer,
                                const char *buf, size_t len);
/* Match a response to a client transaction and run its state machine.
 * Returns false if no transaction matched (stray response). */
bool sip_txn_on_response(sip_txn_layer_t *l, uint64_t now, const sip_msg_t *m);
/* CANCEL for a client INVITE transaction (§9.1). Returns length or -1. */
int sip_txn_build_cancel(const sip_txn_t *t, char *out, size_t cap);
/* A CANCEL for this client INVITE went out: without a final response within
 * 64*T1 the transaction ends (§9.1; on_timeout, as for timer B / C). */
void sip_txn_cancel_sent(sip_txn_t *t, uint64_t now);

/* Server: find the transaction a request belongs to (ACK → INVITE). */
sip_txn_t *sip_txn_server_match(sip_txn_layer_t *l, const sip_msg_t *m);
/* Handle a matched retransmission / ACK. Returns true when consumed. */
bool sip_txn_server_absorb(sip_txn_layer_t *l, uint64_t now, sip_txn_t *t, const sip_msg_t *m);
sip_txn_t *sip_txn_server_create(sip_txn_layer_t *l, uint64_t now, const sip_msg_t *m,
                                 const sip_addr_t *from);
/* Send (and remember for retransmission) a response on a server txn. A
 * terminated transaction (id 0) is ignored. */
void sip_txn_respond(sip_txn_layer_t *l, uint64_t now, sip_txn_t *t, int status,
                     const char *buf, size_t len);
/* The TU received the ACK for a 2xx sent on this server INVITE txn. */
void sip_txn_ack_2xx(sip_txn_t *t);

/* Via sent-by normalized as "host:port" (default port 5060). */
int sip_via_sent_by(const sip_via_t *v, char *out, size_t cap);
