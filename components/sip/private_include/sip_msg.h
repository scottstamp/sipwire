#pragma once

/* SIP message parser (RFC 3261 §7, §20, §25) and bounded output writer.
 *
 * The parser works in place on a mutable buffer (header folding is undone by
 * overwriting the CRLF with spaces) and fills sip_msg_t with slices pointing
 * into that buffer. It never allocates and never reads outside [buf, buf+len).
 * Anything it cannot parse with certainty is rejected. */

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SIP_MAX_MSG_IN     2048
#define SIP_MAX_MSG_OUT    1500
#define SIP_MAX_VIA        8
#define SIP_MAX_CONTACT    8
#define SIP_MAX_ROUTE      8
#define SIP_MAX_AUTH       4
#define SIP_MAX_HEADERS    64
#define SIP_MAX_CSEQ       0x7fffffffu   /* §8.1.1.5: < 2^31 */

typedef struct {
    const char *p;
    size_t len;
} sip_str_t;

#define SIP_STR_NULL ((sip_str_t){ NULL, 0 })

typedef enum {
    SIP_METHOD_UNKNOWN = 0,
    SIP_METHOD_INVITE,
    SIP_METHOD_ACK,
    SIP_METHOD_BYE,
    SIP_METHOD_CANCEL,
    SIP_METHOD_OPTIONS,
    SIP_METHOD_REGISTER,
    SIP_METHOD_INFO,
    SIP_METHOD_UPDATE,
    SIP_METHOD_PRACK,
    SIP_METHOD_SUBSCRIBE,
    SIP_METHOD_NOTIFY,
    SIP_METHOD_REFER,
    SIP_METHOD_MESSAGE,
    SIP_METHOD_PUBLISH,
} sip_method_t;

typedef struct {
    sip_str_t raw;          /* whole via-parm, for copying into responses */
    sip_str_t transport;    /* "UDP" */
    sip_str_t host;         /* sent-by host (IPv6 with brackets) */
    int port;               /* 0 = absent */
    sip_str_t params;       /* ";branch=...;rport" */
    sip_str_t branch;
    sip_str_t received;
    int rport;              /* -1 absent, 0 present without value, else port */
} sip_via_t;

typedef struct {
    sip_str_t raw;
    sip_str_t display;      /* without quotes, still escaped */
    sip_str_t uri;          /* addr-spec */
    sip_str_t params;       /* header params after the URI, starting at ';' */
    sip_str_t tag;
    int expires;            /* Contact expires param, -1 absent */
    bool star;              /* Contact: * */
} sip_nameaddr_t;

typedef struct {
    sip_str_t scheme;
    sip_str_t user;         /* escaped */
    sip_str_t host;
    int port;               /* 0 = absent */
    sip_str_t params;       /* ";lr;transport=udp" */
    sip_str_t headers;      /* after '?' */
    bool lr;
} sip_uri_t;

typedef struct {
    bool is_request;
    /* request line */
    sip_method_t method;
    sip_str_t method_str;
    sip_str_t ruri;
    /* status line */
    int status;
    sip_str_t reason;

    sip_via_t via[SIP_MAX_VIA];
    int n_via;
    sip_nameaddr_t from, to;
    sip_str_t call_id;
    uint32_t cseq;
    sip_method_t cseq_method;
    sip_str_t cseq_method_str;
    sip_nameaddr_t contact[SIP_MAX_CONTACT];
    int n_contact;
    sip_nameaddr_t route[SIP_MAX_ROUTE];
    int n_route;
    sip_nameaddr_t rr[SIP_MAX_ROUTE];
    int n_rr;
    sip_str_t www_auth[SIP_MAX_AUTH];
    int n_www_auth;
    sip_str_t proxy_auth[SIP_MAX_AUTH];
    int n_proxy_auth;
    int content_length;     /* -1 absent */
    sip_str_t content_type;
    int expires;            /* -1 absent */
    int min_expires;        /* -1 absent */
    int max_forwards;       /* -1 absent */
    sip_str_t allow;
    sip_str_t supported;
    sip_str_t require;
    sip_str_t user_agent;
    sip_str_t event;        /* Event (RFC 6665), whole value incl. params */
    sip_str_t sub_state;    /* Subscription-State, whole value */
    int retry_after;        /* Retry-After seconds, -1 absent */
    sip_str_t body;
} sip_msg_t;

typedef enum {
    SIP_PARSE_OK = 0,
    SIP_PARSE_ERR = -1,      /* malformed */
    SIP_PARSE_TOO_BIG = -2,
} sip_parse_result_t;

/* Parse a datagram. buf is modified (unfolding). Leading CRLFs are skipped
 * (§7.5). On UDP, missing Content-Length means "rest of datagram"; a body
 * shorter than Content-Length rejects the message, a longer one is
 * truncated (§18.3). */
sip_parse_result_t sip_msg_parse(sip_msg_t *m, char *buf, size_t len);

/* Name-addr / addr-spec (§20.10, §25.1). allow_star for Contact. */
int sip_nameaddr_parse(sip_str_t s, sip_nameaddr_t *na, bool allow_star);
/* sip:/sips: URI (§19.1.1). Returns -1 for other schemes or malformed. */
int sip_uri_parse(sip_str_t s, sip_uri_t *u);

sip_method_t sip_method_from_str(sip_str_t s);
const char *sip_method_name(sip_method_t m);
const char *sip_reason_phrase(int code);

/* ---- string helpers (all bounded) ---- */
sip_str_t sip_str_c(const char *s);
sip_str_t sip_str_trim(sip_str_t s);
bool sip_str_eq(sip_str_t a, sip_str_t b);
bool sip_str_ieq(sip_str_t a, sip_str_t b);
bool sip_str_ieq_c(sip_str_t a, const char *b);
/* Copy and NUL-terminate. Returns length, or -1 (dst = "") if it won't fit. */
int sip_str_copy(char *dst, size_t cap, sip_str_t s);
bool sip_str_to_u32(sip_str_t s, uint32_t max, uint32_t *out);
/* Find ";name[=value]" in a parameter list (header or URI params). Value
 * quotes are stripped. Returns true if present. */
bool sip_param_find(sip_str_t params, const char *name, sip_str_t *val);
/* Split a comma-separated header value at top level (outside quotes and
 * angle brackets). Returns false at end; *err set on unbalanced input. */
bool sip_list_next(sip_str_t *rest, sip_str_t *item, bool *err);
/* Percent-decode (user part of a URI) into dst. Returns -1 on bad escape or
 * overflow. */
int sip_unescape(char *dst, size_t cap, sip_str_t s);
bool sip_is_token_char(char c);

/* ---- bounded writer ---- */
typedef struct {
    char *buf;
    size_t cap;              /* including room for the NUL */
    size_t len;
    bool overflow;
} sip_wbuf_t;

void sip_w_init(sip_wbuf_t *w, char *buf, size_t cap);
void sip_w_cstr(sip_wbuf_t *w, const char *s);
void sip_w_str(sip_wbuf_t *w, sip_str_t s);
void sip_w_mem(sip_wbuf_t *w, const void *p, size_t n);
void sip_w_fmt(sip_wbuf_t *w, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void sip_w_vfmt(sip_wbuf_t *w, const char *fmt, va_list ap);
