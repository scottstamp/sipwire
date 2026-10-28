#pragma once

/* Minimal SDP (RFC 4566) offer/answer (RFC 3264) for one audio stream:
 * PCMU (0), PCMA (8), telephone-event (RFC 4733), ptime, direction. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sip_ua.h"

#define SIP_SDP_MAX_M        4
#define SIP_SDP_MAX_FMT      16
#define SIP_SDP_MAX_RTPMAP   16

typedef struct {
    char media[12];          /* "audio", "video"... */
    char proto[16];          /* "RTP/AVP" */
    char fmt[8];             /* first format, to mirror in a rejected line */
    uint16_t port;
} sip_sdp_mline_t;

typedef struct {
    uint8_t pt;
    char enc[24];
    uint32_t rate;
} sip_sdp_rtpmap_t;

/* Parsed remote SDP. Self-contained (no pointers into the source). */
typedef struct {
    char o_version[24];
    int n_m;
    sip_sdp_mline_t m[SIP_SDP_MAX_M];
    int audio;               /* index of the usable audio m-line, -1 none */
    uint32_t audio_ip;       /* effective c= for the audio line (host order) */
    bool hold_ip;            /* c=0.0.0.0 (RFC 2543 style hold) */
    uint16_t audio_port;
    uint8_t fmts[SIP_SDP_MAX_FMT];
    int n_fmts;
    sip_sdp_rtpmap_t rtpmap[SIP_SDP_MAX_RTPMAP];
    int n_rtpmap;
    uint8_t ptime;           /* 0 = absent */
    sip_media_dir_t dir;
} sip_sdp_t;

typedef struct {
    uint8_t pt;              /* 0 or 8 */
    int dtmf_pt;             /* -1 none */
    uint8_t ptime;
    uint32_t ip;
    uint16_t port;
    sip_media_dir_t remote_dir; /* as declared by remote (after c=0.0.0.0 fixup) */
} sip_sdp_neg_t;

typedef struct {
    uint32_t ip;
    uint16_t port;
    uint32_t sess_id;
    uint32_t sess_ver;
    const uint8_t *pts;      /* audio payload types to list */
    int n_pts;
    int dtmf_pt;             /* -1 none */
    uint8_t ptime;
    sip_media_dir_t dir;
    const sip_sdp_t *offer;  /* when answering: mirror its m-lines */
} sip_sdp_local_t;

/* Returns 0 on success, -1 if malformed. A body without a usable audio line
 * parses fine with audio == -1. */
int sip_sdp_parse(sip_sdp_t *s, const char *body, size_t len);

/* Pick the codec. answerer=true: first of prefs offered by the remote;
 * answerer=false (remote is an answer): first remote format we support.
 * Returns -1 if there is no common codec or no usable audio line. */
int sip_sdp_negotiate(const sip_sdp_t *r, const uint8_t *prefs, int n_prefs, bool want_dtmf,
                      bool answerer, sip_sdp_neg_t *out);

/* Our direction given what we want and what the remote declared. */
sip_media_dir_t sip_sdp_dir_combine(sip_media_dir_t local_want, sip_media_dir_t remote);

/* Build an SDP body. Returns its length or -1 on overflow. */
int sip_sdp_build(char *out, size_t cap, const sip_sdp_local_t *l);

const char *sip_sdp_dir_name(sip_media_dir_t d);
