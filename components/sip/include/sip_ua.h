#pragma once

/* sipwire SIP user agent (RFC 3261 subset over UDP).
 *
 * The protocol core is pure C17 with no OS dependencies:
 *   - time is injected: call sip_ua_tick(ua, now_ms) every 50-100 ms;
 *   - received datagrams are fed with sip_ua_on_packet();
 *   - datagrams are sent through cfg.send;
 *   - randomness (Call-ID, tags, branches, cnonce) comes from cfg.rand.
 * The optional sip_udp_* glue at the end of this header is the only part that
 * touches sockets (BSD API: lwIP on target, POSIX on host).
 *
 * Threading: a sip_ua_t is not thread safe. Call every function from one task
 * (the "sip" task) or serialize externally.
 *
 * Callbacks: invoked synchronously, but only at the very end of the public
 * entry point that produced them (tick / on_packet / API call), after the
 * internal state is consistent. Callbacks MAY call any sip_ua / sip_call
 * function, including sip_ua_destroy() (destruction is then deferred until
 * the outermost entry point returns; the handle must not be used afterwards).
 * String pointers passed to callbacks are valid only during the callback.
 *
 * Calls: at most two calls are visible to the application at a time (call
 * waiting, attended transfer). A second incoming INVITE is offered (normal
 * incoming_call + RINGING_IN) only while exactly one call exists, that call
 * is established (ACTIVE or HELD) and call waiting is on
 * (sip_ua_set_call_waiting, default on); otherwise it is answered 486 Busy
 * Here internally and reported through cb.call_rejected. With DND on
 * (sip_ua_set_dnd) every new INVITE gets 486 without ringing (also reported
 * through call_rejected). A second outgoing call (sip_call_dial) is allowed
 * while the only other call is established (consultation call).
 *
 * Hold: sip_call_hold() records the wanted state and sends the re-INVITE as
 * soon as no other INVITE transaction of the call is in flight. A 491
 * (glare) is retried after the RFC 3261 §14.1 random delay: 2.1-4 s if we
 * own the Call-ID (we placed the call), else 0-2 s.
 *
 * Transfer (RFC 3515): sip_call_transfer() (blind) and
 * sip_call_transfer_replace() (attended, Refer-To with Replaces, RFC 3891)
 * send a REFER and report progress through cb.transfer: TRYING, ACCEPTED on
 * 202, PROGRESS for 1xx sipfrag NOTIFYs of the implicit subscription,
 * SUCCESS for a 2xx sipfrag, FAILED for a REFER error / timeout, a >= 300
 * sipfrag, or a subscription that terminated without a final status. The
 * call itself is left alone (the application hangs it up on SUCCESS).
 * Incoming REFER is declined (603).
 *
 * DTMF via SIP INFO: sip_call_send_info_dtmf() (application/dtmf-relay) for
 * peers without telephone-event. Incoming INFO DTMF (application/dtmf-relay
 * or application/dtmf) is answered 200 and logged.
 *
 * Auto answer (paging / intercom): an incoming INVITE with
 * "Call-Info: <...>;answer-after=N" or an Alert-Info asking for auto answer
 * ("info=alert-autoanswer", "Ring Answer", "intercom", "auto answer",
 * optional ";delay=N") reports answer_after = N (seconds, else -1) in
 * sip_call_info_t. The UA does not answer by itself.
 *
 * Subscriptions (RFC 6665): up to SIP_UA_MAX_BLF "dialog" subscriptions (BLF,
 * RFC 4235) and one "message-summary" subscription (MWI, RFC 3842). They are
 * sent once the registration is up, refreshed, re-established with backoff
 * and reported through the blf_state / mwi callbacks. Unsolicited
 * message-summary NOTIFYs from the server are reported through mwi as well.
 *
 * Functions return sip_err_t (0 = OK, negative = error); the phone component
 * maps these to esp_err_t. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SIP_UA_USER_AGENT   "sipwire/0.1"
#define SIP_UA_MAX_CODECS   4
#define SIP_UA_MAX_BLF      8                   /* "dialog" subscriptions */
#define SIP_UA_MAX_SUBS     (SIP_UA_MAX_BLF + 1) /* + one "message-summary" */
#define SIP_UA_EXT_MAX      32                  /* BLF target incl. NUL */

typedef enum {
    SIP_OK            = 0,
    SIP_ERR_ARG       = -1,    /* invalid argument / config */
    SIP_ERR_NOMEM     = -2,    /* allocation or fixed pool exhausted */
    SIP_ERR_STATE     = -3,    /* not allowed in the current state */
    SIP_ERR_BUSY      = -4,    /* a call already exists */
    SIP_ERR_NOT_FOUND = -5,    /* unknown call handle */
    SIP_ERR_OVERFLOW  = -6,    /* message would exceed the fixed buffer */
    SIP_ERR_TRANSPORT = -7,    /* socket error */
} sip_err_t;

/* IPv4 address + UDP port. ip is in HOST byte order (127.0.0.1 == 0x7f000001). */
typedef struct {
    uint32_t ip;
    uint16_t port;
} sip_addr_t;

/* Same ordering as ESP_LOG levels: E W I D V. */
typedef enum {
    SIP_LOG_NONE = 0,
    SIP_LOG_ERROR,
    SIP_LOG_WARN,
    SIP_LOG_INFO,
    SIP_LOG_DEBUG,
    SIP_LOG_VERBOSE,           /* full SIP messages (auth headers redacted) */
} sip_log_level_t;

/* Registration state. Same order/meaning as sip_reg_state_t in
 * sipwire_events.h, so the phone can cast. */
typedef enum {
    SIP_UA_REG_NONE,
    SIP_UA_REG_REGISTERING,
    SIP_UA_REG_REGISTERED,
    SIP_UA_REG_FAILED,         /* retried automatically with backoff */
} sip_ua_reg_state_t;

/* Call state. Same order/meaning as call_state_t in sipwire_events.h. */
typedef enum {
    SIP_UA_CALL_IDLE,
    SIP_UA_CALL_DIALING,       /* INVITE sent, no 180/183 yet */
    SIP_UA_CALL_RINGING_OUT,   /* 180/183 received */
    SIP_UA_CALL_RINGING_IN,    /* incoming INVITE, 180 sent */
    SIP_UA_CALL_ACTIVE,
    SIP_UA_CALL_HELD,          /* held locally (sip_call_hold) */
    SIP_UA_CALL_ENDED,         /* final; handle is invalid after the callback */
} sip_ua_call_state_t;

typedef enum {
    SIP_END_NONE,
    SIP_END_LOCAL,             /* we hung up, cancelled or rejected */
    SIP_END_REMOTE,            /* remote sent BYE */
    SIP_END_REJECTED,          /* our INVITE got a 3xx-6xx (status_code) */
    SIP_END_CANCELLED,         /* remote cancelled the incoming call */
    SIP_END_TIMEOUT,           /* no response / no ACK (status_code 408) */
    SIP_END_ERROR,             /* e.g. no common codec (status_code 488) */
} sip_ua_end_reason_t;

/* Media direction from OUR point of view (SENDONLY = we send, peer won't). */
typedef enum {
    SIP_DIR_SENDRECV,
    SIP_DIR_SENDONLY,
    SIP_DIR_RECVONLY,
    SIP_DIR_INACTIVE,
} sip_media_dir_t;

typedef enum {
    SIP_KEEPALIVE_CRLF,        /* RFC 5626 style double-CRLF ping */
    SIP_KEEPALIVE_OPTIONS,     /* OPTIONS to the server; timeout → re-register */
} sip_keepalive_mode_t;

typedef int sip_call_handle_t; /* > 0 when valid */
typedef int sip_sub_handle_t;  /* > 0 when valid */

/* Busy lamp state of a watched extension (dialog package, RFC 4235). */
typedef enum {
    SIP_BLF_UNKNOWN,           /* not (yet) subscribed, rejected or failed */
    SIP_BLF_IDLE,              /* no dialogs, or all terminated */
    SIP_BLF_RINGING,           /* a dialog in "early" state (being called) */
    SIP_BLF_BUSY,              /* a confirmed dialog (or placing a call) */
} sip_blf_state_t;

typedef struct {
    sip_ua_call_state_t state;
    sip_ua_end_reason_t end_reason;   /* valid in SIP_UA_CALL_ENDED */
    int status_code;                  /* last relevant SIP status, 0 = none */
    bool outgoing;
    const char *number;               /* remote user part, unescaped */
    const char *display_name;         /* may be "" */
    int answer_after;                 /* incoming: auto answer requested after N s, -1 = no */
} sip_call_info_t;

/* Transfer progress (cb.transfer). */
typedef enum {
    SIP_XFER_TRYING,                  /* REFER sent */
    SIP_XFER_ACCEPTED,                /* 202 Accepted */
    SIP_XFER_PROGRESS,                /* NOTIFY sipfrag 1xx (code) */
    SIP_XFER_SUCCESS,                 /* NOTIFY sipfrag 2xx (code) */
    SIP_XFER_FAILED,                  /* code: REFER status, sipfrag status, 408, 0 = unknown */
} sip_xfer_state_t;

typedef struct {
    uint32_t remote_ip;               /* host byte order */
    char remote_ip_str[16];
    uint16_t remote_port;             /* remote RTP port */
    uint16_t local_port;              /* our RTP port (as offered/answered) */
    uint8_t payload_type;             /* 9 = G.722, 0 = PCMU, 8 = PCMA */
    int16_t dtmf_pt;                  /* telephone-event PT, -1 if none */
    uint8_t ptime_ms;                 /* packetization to send with */
    sip_media_dir_t direction;
    bool early;                       /* from a 183 (early media) */
} sip_media_info_t;

typedef struct {
    void (*reg_state)(void *ctx, sip_ua_reg_state_t state, int status_code);
    /* Followed by call_state(RINGING_IN). Answer/reject with the handle. */
    void (*incoming_call)(void *ctx, sip_call_handle_t call,
                          const char *number, const char *display_name);
    void (*call_state)(void *ctx, sip_call_handle_t call, const sip_call_info_t *info);
    /* Negotiated media became available or changed (answer, re-INVITE). */
    void (*media)(void *ctx, sip_call_handle_t call, const sip_media_info_t *media);
    /* BLF state of a watched extension changed (also UNKNOWN when the
     * subscription failed or ended and is being retried). */
    void (*blf_state)(void *ctx, sip_sub_handle_t sub, const char *ext, sip_blf_state_t state);
    /* Message waiting indication (subscription or unsolicited NOTIFY),
     * reported when it changes. new_msgs / old_msgs: voice messages. */
    void (*mwi)(void *ctx, bool waiting, int new_msgs, int old_msgs);
    /* Transfer (REFER) of `call` progressed. */
    void (*transfer)(void *ctx, sip_call_handle_t call, sip_xfer_state_t state, int code);
    /* An incoming INVITE was refused without being offered (busy, DND). */
    void (*call_rejected)(void *ctx, const char *number, const char *display_name, int code);
    void *ctx;
} sip_ua_callbacks_t;

/* Send one datagram. Return 0 on success, negative on error. */
typedef int (*sip_ua_send_fn)(void *ctx, const sip_addr_t *to, const void *data, size_t len);
typedef uint32_t (*sip_ua_rand_fn)(void *ctx);
/* msg is a NUL-terminated line without trailing newline. */
typedef void (*sip_ua_log_fn)(void *ctx, sip_log_level_t level, const char *msg);
/* Return the local RTP port for a new call (even number). */
typedef uint16_t (*sip_ua_rtp_port_fn)(void *ctx);

typedef struct {
    /* Server / registrar. All requests go to server_addr (outbound-proxy
     * style); server_host:server_port is used in URIs. */
    const char *server_host;          /* hostname or IPv4 literal */
    uint16_t server_port;             /* 0 → 5060 */
    sip_addr_t server_addr;           /* ip 0 → parse server_host as IPv4 */
    const char *domain;               /* AOR domain; NULL → server_host */

    /* Account */
    const char *user;
    const char *auth_user;            /* NULL/"" → user */
    const char *password;
    const char *display_name;         /* NULL/"" → none */
    uint32_t expires;                 /* REGISTER expires (s); 0 → 300 */

    /* Local addresses */
    sip_addr_t local_addr;            /* our IP + bound SIP UDP port */
    uint16_t rtp_port;                /* used when rtp_port_alloc is NULL */
    sip_ua_rtp_port_fn rtp_port_alloc;
    void *rtp_port_ctx;

    /* Media: static payload types in preference order, no duplicates
     * (9 = G.722, 0 = PCMU, 8 = PCMA); n_codecs == 0 → {9, 0, 8}. The offer
     * lists them in this order; an answer picks the first one the offer
     * contains. All use an 8000 Hz RTP clock ("G722/8000", RFC 3551).
     * dtmf_pt 0 → 101, < 0 → no telephone-event. */
    uint8_t codecs[SIP_UA_MAX_CODECS];
    uint8_t n_codecs;
    int16_t dtmf_pt;
    uint8_t ptime_ms;                 /* 0 → 20 */

    /* NAT */
    uint32_t keepalive_s;             /* 0 = off */
    sip_keepalive_mode_t keepalive_mode;
    bool disable_nat_contact_fix;     /* default: use Via received/rport */

    /* Security: by default datagrams whose source IP is not server_addr.ip
     * are dropped (no rogue INVITEs from the LAN). */
    bool accept_any_source;

    /* Subscriptions: requested Expires (s); 0 → 600. */
    uint32_t sub_expires;

    /* Hooks (send and rand are mandatory) */
    sip_ua_send_fn send;
    void *send_ctx;
    sip_ua_rand_fn rand;
    void *rand_ctx;
    sip_ua_log_fn log;
    void *log_ctx;
    sip_log_level_t log_level;        /* 0 → SIP_LOG_INFO */

    sip_ua_callbacks_t cb;
} sip_ua_config_t;

typedef struct sip_ua sip_ua_t;

/* Allocates the UA once (fixed pools inside, ~70 KB; nothing else is
 * allocated afterwards). Strings are copied. */
sip_err_t sip_ua_create(const sip_ua_config_t *cfg, sip_ua_t **out);

/* Frees immediately (or at the end of the current entry point when called
 * from a callback). Sends nothing: call sip_ua_unregister() and tick until
 * SIP_UA_REG_NONE first for a clean shutdown. */
void sip_ua_destroy(sip_ua_t *ua);

/* Advance the virtual clock (monotonic milliseconds) and run timers. Call
 * once before anything else and then every 50-100 ms. */
void sip_ua_tick(sip_ua_t *ua, uint64_t now_ms);

/* Feed one received datagram. Malformed or oversized input is dropped. */
void sip_ua_on_packet(sip_ua_t *ua, const void *data, size_t len, const sip_addr_t *from);

/* Start registering and keep the registration refreshed (and retried with
 * backoff on failure) until sip_ua_unregister(). */
sip_err_t sip_ua_register(sip_ua_t *ua);

/* REGISTER with Expires: 0; state goes to NONE on response or timeout.
 * Also ends every subscription (un-SUBSCRIBE with Expires: 0 where one is
 * established); their handles become invalid, no further callbacks. */
sip_err_t sip_ua_unregister(sip_ua_t *ua);

sip_ua_reg_state_t sip_ua_reg_state(const sip_ua_t *ua);

/* Local IP/port changed (e.g. Wi-Fi reconnect): re-register if registered. */
sip_err_t sip_ua_set_local_addr(sip_ua_t *ua, const sip_addr_t *local);

/* Place a call. number: dialable digits/user part (escaped as needed), or a
 * full "sip:" URI. Returns SIP_ERR_BUSY if two calls exist, or one that is
 * not established. */
sip_err_t sip_call_dial(sip_ua_t *ua, const char *number, sip_call_handle_t *out);

/* Answer an incoming ringing call (200 OK with SDP answer). */
sip_err_t sip_call_answer(sip_ua_t *ua, sip_call_handle_t call);

/* Reject an incoming ringing call with a 4xx-6xx code (486, 603, ...). */
sip_err_t sip_call_reject(sip_ua_t *ua, sip_call_handle_t call, int code);

/* End the call whatever its state: CANCEL (outgoing, early), 486 (incoming
 * ringing) or BYE (established). ENDED is reported immediately; protocol
 * cleanup (waiting for 487, 200/CANCEL race → ACK+BYE) continues internally. */
sip_err_t sip_call_hangup(sip_ua_t *ua, sip_call_handle_t call);

/* Local hold/resume via re-INVITE (a=sendonly / a=sendrecv). Queued while
 * another INVITE transaction of the call is in flight; the state changes to
 * HELD / ACTIVE on the 2xx. SIP_ERR_STATE if the call is not established. */
sip_err_t sip_call_hold(sip_ua_t *ua, sip_call_handle_t call, bool hold);

/* Blind transfer: REFER with Refer-To: <sip:target@domain> (or a full sip:
 * URI). One transfer per call at a time (SIP_ERR_BUSY). */
sip_err_t sip_call_transfer(sip_ua_t *ua, sip_call_handle_t call, const char *target);

/* Attended transfer: REFER on `call` with Refer-To: <consult remote URI
 * ?Replaces=call-id;to-tag=..;from-tag=..> naming the established `consult`
 * dialog. */
sip_err_t sip_call_transfer_replace(sip_ua_t *ua, sip_call_handle_t call, sip_call_handle_t consult);

/* One DTMF digit as SIP INFO (application/dtmf-relay, Signal= / Duration=). */
sip_err_t sip_call_send_info_dtmf(sip_ua_t *ua, sip_call_handle_t call, char digit, unsigned duration_ms);

/* Incoming call policy (default: call waiting on, DND off). */
void sip_ua_set_call_waiting(sip_ua_t *ua, bool on);
void sip_ua_set_dnd(sip_ua_t *ua, bool on);

/* Current state of a call, SIP_UA_CALL_IDLE if unknown. */
sip_ua_call_state_t sip_call_state(const sip_ua_t *ua, sip_call_handle_t call);

/* ---- subscriptions ---- */

/* Watch extension `ext` (user part, or a full "sip:" URI) with the "dialog"
 * event package. The SUBSCRIBE goes out once registered (at most a few in
 * flight at a time); state changes come through cb.blf_state, starting with
 * UNKNOWN. SIP_ERR_NOMEM when SIP_UA_MAX_BLF are in use, SIP_ERR_ARG for an
 * empty / too long / undialable ext. */
sip_err_t sip_ua_subscribe_blf(sip_ua_t *ua, const char *ext, sip_sub_handle_t *out);

/* Subscribe to our own mailbox ("message-summary" to our AOR). One at a
 * time (SIP_ERR_BUSY if it exists). out may be NULL. */
sip_err_t sip_ua_subscribe_mwi(sip_ua_t *ua, sip_sub_handle_t *out);

/* End a subscription: un-SUBSCRIBE (Expires: 0) if established. The handle
 * is invalid afterwards and reports nothing more. */
sip_err_t sip_ua_unsubscribe(sip_ua_t *ua, sip_sub_handle_t sub);

/* Last reported BLF state, SIP_BLF_UNKNOWN for an unknown handle. */
sip_blf_state_t sip_ua_blf_state(const sip_ua_t *ua, sip_sub_handle_t sub);

/* "a.b.c.d" helpers (host byte order). */
bool sip_ip_parse(const char *s, uint32_t *ip);
void sip_ip_format(uint32_t ip, char out[16]);

/* ---- Optional UDP transport glue (sip_udp.c, BSD sockets) ---- */

typedef struct {
    int fd;
    uint16_t port;                    /* bound local port */
} sip_udp_t;

/* Bind 0.0.0.0:port (0 = ephemeral), non-blocking. */
sip_err_t sip_udp_open(sip_udp_t *u, uint16_t port);
void sip_udp_close(sip_udp_t *u);

/* Matches sip_ua_send_fn; ctx = sip_udp_t*. */
int sip_udp_send(void *ctx, const sip_addr_t *to, const void *data, size_t len);

/* Wait up to timeout_ms for datagrams and feed all pending ones to the UA.
 * Does not tick. Returns number of datagrams fed or a negative error. */
int sip_udp_poll(sip_udp_t *u, sip_ua_t *ua, int timeout_ms);

/* Resolve host (IPv4 literal or DNS name) to an address. */
sip_err_t sip_udp_resolve(const char *host, uint16_t port, sip_addr_t *out);

/* Local source IP the stack would use to reach dest (UDP connect trick). */
sip_err_t sip_udp_local_ip_for(const sip_addr_t *dest, uint32_t *ip);

#ifdef __cplusplus
}
#endif
