#pragma once

/* SIP event packages (RFC 6665) used by the UA's subscriptions: header
 * parsing for Event / Subscription-State and the two bodies we understand:
 *   - application/dialog-info+xml (RFC 4235, "dialog" package, BLF)
 *   - application/simple-message-summary (RFC 3842, "message-summary", MWI)
 * Pure C, no allocation, never reads outside [body, body + len). The XML
 * scanner is a small non-recursive tokenizer for exactly this document type,
 * not a general XML parser: anything it does not understand with certainty
 * (DOCTYPE, CDATA, unbalanced tags, deep nesting, oversized input) is
 * rejected. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sip_msg.h"

/* ---- Event / Subscription-State headers ---------------------------------- */

typedef enum {
    SIP_SUBST_ACTIVE,
    SIP_SUBST_PENDING,
    SIP_SUBST_TERMINATED,
    SIP_SUBST_OTHER,          /* unknown substate value: treat like active/pending */
} sip_substate_t;

typedef enum {
    SIP_SUBR_NONE,            /* no reason given */
    SIP_SUBR_DEACTIVATED,     /* retry immediately (RFC 6665 §4.1.3) */
    SIP_SUBR_PROBATION,       /* retry later (retry-after) */
    SIP_SUBR_REJECTED,        /* don't retry (we back off long) */
    SIP_SUBR_TIMEOUT,         /* retry immediately */
    SIP_SUBR_GIVEUP,          /* retry later (retry-after) */
    SIP_SUBR_NORESOURCE,      /* don't retry (we back off long) */
    SIP_SUBR_INVARIANT,       /* don't retry (we back off long) */
    SIP_SUBR_OTHER,
} sip_subreason_t;

typedef struct {
    sip_substate_t state;
    sip_subreason_t reason;
    int expires;              /* -1 absent */
    int retry_after;          /* -1 absent */
} sip_sub_state_t;

/** Parse a Subscription-State value ("active;expires=600",
 *  "terminated;reason=noresource;retry-after=30"). Returns 0 or -1. */
int sip_sub_state_parse(sip_str_t v, sip_sub_state_t *out);

/** Event header value: package name (before any ';') and the id param. */
int sip_event_parse(sip_str_t v, sip_str_t *package, sip_str_t *id);

/* ---- RFC 4235 dialog-info+xml ---------------------------------------------- */

#define SIP_DLGINFO_MAX_DIALOGS  8
#define SIP_DLGINFO_ID_MAX       48
#define SIP_DLGINFO_MAX_LEN      4096   /* larger bodies are rejected */
#define SIP_DLGINFO_MAX_DEPTH    16

typedef enum {
    SIP_DLG_TRYING,
    SIP_DLG_PROCEEDING,
    SIP_DLG_EARLY,
    SIP_DLG_CONFIRMED,
    SIP_DLG_TERMINATED,
    SIP_DLG_UNKNOWN,          /* missing or unknown <state> text */
} sip_dlg_state_t;

typedef enum {
    SIP_DLGDIR_NONE,
    SIP_DLGDIR_INITIATOR,     /* the watched party placed the call */
    SIP_DLGDIR_RECIPIENT,     /* the watched party is being called */
} sip_dlg_dir_t;

typedef struct {
    char id[SIP_DLGINFO_ID_MAX];      /* truncated if longer */
    sip_dlg_state_t state;
    sip_dlg_dir_t dir;
} sip_dlginfo_dialog_t;

typedef struct {
    uint32_t version;
    bool full;                        /* state="full" (else "partial") */
    int n;                            /* stored dialogs (<= MAX) */
    int n_total;                      /* dialogs in the document */
    sip_dlginfo_dialog_t d[SIP_DLGINFO_MAX_DIALOGS];
} sip_dlginfo_t;

/** Parse a dialog-info document. Returns 0, or -1 when it is malformed,
 *  truncated, too large/deep, or not a dialog-info document. */
int sip_dlginfo_parse(const char *xml, size_t len, sip_dlginfo_t *out);

/* ---- RFC 3842 message-summary -------------------------------------------------- */

typedef struct {
    bool waiting;                     /* Messages-Waiting: yes */
    uint32_t new_msgs, old_msgs;      /* Voice-Message: new/old */
    uint32_t new_urgent, old_urgent;  /* Voice-Message: ... (new/old) */
    bool have_voice;                  /* a Voice-Message line was present */
} sip_msgsum_t;

/** Parse a simple-message-summary body. Returns 0, or -1 if the mandatory
 *  Messages-Waiting line is missing or malformed. Counts are capped at
 *  999999. */
int sip_msgsum_parse(const char *body, size_t len, sip_msgsum_t *out);

/** Content-Type check without parameters, case-insensitive. */
bool sip_content_type_is(sip_str_t ct, const char *type);
