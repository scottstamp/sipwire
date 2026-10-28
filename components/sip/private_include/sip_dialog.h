#pragma once

/* SIP dialog state (RFC 3261 §12): Call-ID, tags, remote target, route set,
 * CSeq numbering and in-dialog request construction (loose and strict
 * routing, §12.2.1.1). */

#include <stdbool.h>
#include <stdint.h>

#include "sip_msg.h"

#define SIP_CALLID_MAX     128
#define SIP_TAG_MAX        72
#define SIP_URI_MAX        192
#define SIP_NAMEADDR_MAX   256
#define SIP_ROUTESET_MAX   640

typedef struct {
    char call_id[SIP_CALLID_MAX];
    char local_tag[SIP_TAG_MAX];
    char remote_tag[SIP_TAG_MAX];
    char local_na[SIP_NAMEADDR_MAX];    /* our name-addr, without tag */
    char remote_na[SIP_NAMEADDR_MAX];   /* peer name-addr, without tag */
    char remote_target[SIP_URI_MAX];    /* from Contact */
    char routes[SIP_ROUTESET_MAX];      /* route set: NUL-separated name-addrs */
    uint16_t routes_len;
    uint8_t n_routes;
    uint32_t local_cseq;
    uint32_t remote_cseq;
    bool remote_cseq_valid;
    bool confirmed;                     /* false = early */
} sip_dialog_t;

/* Reformat a parsed name-addr without header params: "\"Name\" <uri>". */
int sip_nameaddr_format(char *out, size_t cap, const sip_nameaddr_t *na);

/* UAS side, from a dialog-creating request (§12.1.1). */
int sip_dialog_init_uas(sip_dialog_t *d, const sip_msg_t *req, const char *local_tag);

/* UAC side, from a 1xx-with-tag or 2xx response (§12.1.2). The caller passes
 * what it sent in the request. */
int sip_dialog_init_uac(sip_dialog_t *d, const sip_msg_t *resp, const char *call_id,
                        const char *local_na, const char *local_tag, const char *remote_na,
                        const char *request_uri, uint32_t local_cseq);

/* Target refresh from the Contact of a request/response (§12.2). */
int sip_dialog_set_target(sip_dialog_t *d, const sip_msg_t *m);

/* Does an incoming in-dialog request belong to d? (Call-ID + both tags) */
bool sip_dialog_matches_request(const sip_dialog_t *d, const sip_msg_t *req);

/* Request line, Via, Max-Forwards, From, To, Call-ID, CSeq and Route for an
 * in-dialog request. The caller appends the rest. */
void sip_dialog_write_request(const sip_dialog_t *d, sip_wbuf_t *w, const char *method,
                              uint32_t cseq, const char *sent_by, const char *branch);
