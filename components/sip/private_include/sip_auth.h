#pragma once

/* HTTP Digest authentication for SIP (RFC 3261 §22, RFC 2617).
 * Supports algorithm=MD5 (or absent), qop=auth and the legacy no-qop
 * (RFC 2069) form. MD5-sess, auth-int and SHA-256 challenges are skipped. */

#include <stdbool.h>
#include <stdint.h>

#include "sip_msg.h"

#define SIP_AUTH_REALM_MAX   96
#define SIP_AUTH_NONCE_MAX   160
#define SIP_AUTH_OPAQUE_MAX  160

typedef struct {
    bool valid;
    bool proxy;              /* 407 → Proxy-Authorization */
    bool qop_auth;
    bool stale;
    bool has_opaque;
    char realm[SIP_AUTH_REALM_MAX];
    char nonce[SIP_AUTH_NONCE_MAX];
    char opaque[SIP_AUTH_OPAQUE_MAX];
    uint32_t nc;             /* nonce count of the last use */
} sip_auth_t;

/* Parse one WWW-/Proxy-Authenticate value. Returns 0 if it is a usable
 * Digest MD5 challenge. */
int sip_auth_parse_challenge(sip_str_t value, bool proxy, sip_auth_t *out);

/* Pick the first usable challenge from a 401/407 response. */
int sip_auth_from_response(const sip_msg_t *m, sip_auth_t *out);

/* response = KD(H(A1), nonce:nc:cnonce:qop:H(A2)) or KD(H(A1), nonce:H(A2))
 * when qop is NULL (RFC 2617 §3.2.2.1). out: 32 hex chars + NUL. */
void sip_digest_response(const char *user, const char *realm, const char *pass,
                         const char *method, const char *uri, const char *nonce,
                         const char *nc, const char *cnonce, const char *qop, char out[33]);

/* Append "Authorization: Digest ...\r\n" (or Proxy-Authorization) for the
 * request; increments a->nc. */
void sip_auth_write_header(sip_wbuf_t *w, sip_auth_t *a, const char *user, const char *pass,
                           const char *method, const char *uri, const char *cnonce);
