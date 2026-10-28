/* SDP parse / build / negotiate. See sip_sdp.h. */

#include "sip_sdp.h"

#include <string.h>

#include "sip_msg.h"

static const char *static_enc(uint8_t pt)
{
    switch (pt) {
    case 0: return "PCMU";
    case 8: return "PCMA";
    default: return NULL;
    }
}

const char *sip_sdp_dir_name(sip_media_dir_t d)
{
    switch (d) {
    case SIP_DIR_SENDONLY: return "sendonly";
    case SIP_DIR_RECVONLY: return "recvonly";
    case SIP_DIR_INACTIVE: return "inactive";
    default: return "sendrecv";
    }
}

/* direction <-> bit set (1 = send, 2 = recv) */
static unsigned dir_bits(sip_media_dir_t d)
{
    switch (d) {
    case SIP_DIR_SENDONLY: return 1;
    case SIP_DIR_RECVONLY: return 2;
    case SIP_DIR_INACTIVE: return 0;
    default: return 3;
    }
}

static sip_media_dir_t bits_dir(unsigned b)
{
    static const sip_media_dir_t map[4] = {
        SIP_DIR_INACTIVE, SIP_DIR_SENDONLY, SIP_DIR_RECVONLY, SIP_DIR_SENDRECV
    };
    return map[b & 3];
}

sip_media_dir_t sip_sdp_dir_combine(sip_media_dir_t local_want, sip_media_dir_t remote)
{
    /* remote sending == us receiving: swap the remote's bits (RFC 3264 §6.1) */
    unsigned r = dir_bits(remote);
    unsigned flipped = ((r & 1) << 1) | ((r & 2) >> 1);
    return bits_dir(dir_bits(local_want) & flipped);
}

static bool parse_dir(sip_str_t v, sip_media_dir_t *d)
{
    if (sip_str_eq(v, sip_str_c("sendrecv"))) {
        *d = SIP_DIR_SENDRECV;
    } else if (sip_str_eq(v, sip_str_c("sendonly"))) {
        *d = SIP_DIR_SENDONLY;
    } else if (sip_str_eq(v, sip_str_c("recvonly"))) {
        *d = SIP_DIR_RECVONLY;
    } else if (sip_str_eq(v, sip_str_c("inactive"))) {
        *d = SIP_DIR_INACTIVE;
    } else {
        return false;
    }
    return true;
}

/* next space-separated field */
static bool next_field(sip_str_t *rest, sip_str_t *f)
{
    const char *p = rest->p;
    const char *e = rest->p + rest->len;
    while (p < e && *p == ' ') {
        p++;
    }
    if (p >= e) {
        return false;
    }
    const char *f0 = p;
    while (p < e && *p != ' ') {
        p++;
    }
    *f = (sip_str_t){ f0, (size_t)(p - f0) };
    rest->p = p;
    rest->len = (size_t)(e - p);
    return true;
}

/* "IN IP4 a.b.c.d[/ttl]" → ip; returns false on IP6/garbage */
static bool parse_conn(sip_str_t v, uint32_t *ip)
{
    sip_str_t nt, at, addr;
    if (!next_field(&v, &nt) || !next_field(&v, &at) || !next_field(&v, &addr)) {
        return false;
    }
    if (!sip_str_eq(nt, sip_str_c("IN")) || !sip_str_eq(at, sip_str_c("IP4"))) {
        return false;
    }
    const char *slash = memchr(addr.p, '/', addr.len);
    if (slash) {
        addr.len = (size_t)(slash - addr.p);
    }
    char tmp[16];
    if (sip_str_copy(tmp, sizeof(tmp), addr) < 0) {
        return false;
    }
    return sip_ip_parse(tmp, ip);
}

int sip_sdp_parse(sip_sdp_t *s, const char *body, size_t len)
{
    memset(s, 0, sizeof(*s));
    s->audio = -1;
    s->dir = SIP_DIR_SENDRECV;

    sip_media_dir_t sess_dir = SIP_DIR_SENDRECV;
    sip_media_dir_t m_dir = SIP_DIR_SENDRECV;
    bool m_dir_set = false;
    bool sess_c = false, sess_c_ok = false, m_c = false, m_c_ok = false;
    uint32_t sess_ip = 0, m_ip = 0;
    int cur = -1;
    bool first = true;
    const char *p = body;
    const char *e = body + len;

    while (p < e) {
        const char *nl = memchr(p, '\n', (size_t)(e - p));
        const char *le = nl ? nl : e;
        sip_str_t line = { p, (size_t)(le - p) };
        p = nl ? nl + 1 : e;
        if (line.len && line.p[line.len - 1] == '\r') {
            line.len--;
        }
        if (line.len == 0) {
            continue;
        }
        for (size_t i = 0; i < line.len; i++) {
            if (line.p[i] == '\0' || line.p[i] == '\r') {
                return -1;
            }
        }
        if (line.len < 2 || line.p[1] != '=') {
            return -1;
        }
        char type = line.p[0];
        sip_str_t v = { line.p + 2, line.len - 2 };
        if (first) {
            if (type != 'v' || !sip_str_eq(v, sip_str_c("0"))) {
                return -1;
            }
            first = false;
            continue;
        }
        switch (type) {
        case 'o': {
            sip_str_t f, rest = v;
            for (int i = 0; i < 3; i++) {
                if (!next_field(&rest, &f)) {
                    return -1;
                }
            }
            if (sip_str_copy(s->o_version, sizeof(s->o_version), f) < 0) {
                return -1;
            }
            break;
        }
        case 'c': {
            uint32_t ip = 0;
            bool ok = parse_conn(v, &ip);
            if (cur < 0) {
                sess_c = true;
                sess_c_ok = ok;
                sess_ip = ip;
            } else if (cur == s->audio) {
                m_c = true;
                m_c_ok = ok;
                m_ip = ip;
            }
            break;
        }
        case 'm': {
            if (s->n_m >= SIP_SDP_MAX_M) {
                return -1;
            }
            sip_sdp_mline_t *ml = &s->m[s->n_m];
            sip_str_t media, port, proto, fmt, rest = v;
            if (!next_field(&rest, &media) || !next_field(&rest, &port) ||
                !next_field(&rest, &proto)) {
                return -1;
            }
            const char *slash = memchr(port.p, '/', port.len);
            if (slash) {
                port.len = (size_t)(slash - port.p); /* port/count: ignore count */
            }
            uint32_t pn;
            if (!sip_str_to_u32(port, 65535, &pn) || sip_str_copy(ml->media, sizeof(ml->media), media) < 0 ||
                sip_str_copy(ml->proto, sizeof(ml->proto), proto) < 0) {
                return -1;
            }
            ml->port = (uint16_t)pn;
            sip_str_t fmts = rest;
            if (!next_field(&fmts, &fmt) || sip_str_copy(ml->fmt, sizeof(ml->fmt), fmt) < 0) {
                return -1;
            }
            cur = s->n_m++;
            if (s->audio < 0 && strcmp(ml->media, "audio") == 0 && pn > 0 &&
                strcmp(ml->proto, "RTP/AVP") == 0) {
                s->audio = cur;
                s->audio_port = (uint16_t)pn;
                sip_str_t f;
                while (next_field(&rest, &f)) {
                    uint32_t pt;
                    if (!sip_str_to_u32(f, 127, &pt)) {
                        return -1;
                    }
                    if (s->n_fmts < SIP_SDP_MAX_FMT) {
                        s->fmts[s->n_fmts++] = (uint8_t)pt;
                    }
                }
            }
            break;
        }
        case 'a': {
            if (cur >= 0 && cur != s->audio) {
                break;
            }
            sip_media_dir_t d;
            if (parse_dir(v, &d)) {
                if (cur < 0) {
                    sess_dir = d;
                } else {
                    m_dir = d;
                    m_dir_set = true;
                }
                break;
            }
            if (cur < 0) {
                break;
            }
            if (v.len > 7 && memcmp(v.p, "rtpmap:", 7) == 0) {
                /* a=rtpmap:<pt> <enc>/<rate>[/<ch>] */
                sip_str_t rest = { v.p + 7, v.len - 7 }, ptf, encf;
                uint32_t pt, rate;
                if (!next_field(&rest, &ptf) || !next_field(&rest, &encf) ||
                    !sip_str_to_u32(ptf, 127, &pt)) {
                    return -1;
                }
                const char *sl = memchr(encf.p, '/', encf.len);
                if (!sl) {
                    return -1;
                }
                sip_str_t ename = { encf.p, (size_t)(sl - encf.p) };
                sip_str_t rs = { sl + 1, (size_t)(encf.p + encf.len - sl - 1) };
                const char *sl2 = memchr(rs.p, '/', rs.len);
                if (sl2) {
                    rs.len = (size_t)(sl2 - rs.p);
                }
                if (!sip_str_to_u32(rs, 1000000, &rate)) {
                    return -1;
                }
                if (s->n_rtpmap < SIP_SDP_MAX_RTPMAP) {
                    sip_sdp_rtpmap_t *rm = &s->rtpmap[s->n_rtpmap];
                    if (sip_str_copy(rm->enc, sizeof(rm->enc), ename) >= 0) {
                        rm->pt = (uint8_t)pt;
                        rm->rate = rate;
                        s->n_rtpmap++;
                    }
                }
            } else if (v.len > 6 && memcmp(v.p, "ptime:", 6) == 0) {
                uint32_t pt;
                if (sip_str_to_u32(sip_str_trim((sip_str_t){ v.p + 6, v.len - 6 }), 200, &pt)) {
                    s->ptime = (uint8_t)pt;
                }
            }
            break;
        }
        default:
            break; /* s=, t=, b=, k=, i=, u=, e=, p=, z=, r= ignored */
        }
    }
    if (first) {
        return -1;
    }
    if (s->audio >= 0) {
        bool have = m_c ? m_c_ok : (sess_c && sess_c_ok);
        if (!have) {
            return -1; /* c= is mandatory and must be IN IP4 */
        }
        s->audio_ip = m_c ? m_ip : sess_ip;
        s->hold_ip = s->audio_ip == 0;
        s->dir = m_dir_set ? m_dir : sess_dir;
    }
    return 0;
}

/* encoding name of a remote format: rtpmap wins, static table otherwise */
static bool fmt_enc(const sip_sdp_t *r, uint8_t pt, const char **enc, uint32_t *rate)
{
    for (int i = 0; i < r->n_rtpmap; i++) {
        if (r->rtpmap[i].pt == pt) {
            *enc = r->rtpmap[i].enc;
            *rate = r->rtpmap[i].rate;
            return true;
        }
    }
    *enc = static_enc(pt);
    *rate = 8000;
    return *enc != NULL;
}

static bool remote_supports(const sip_sdp_t *r, uint8_t pt)
{
    const char *want = static_enc(pt);
    for (int i = 0; i < r->n_fmts; i++) {
        const char *enc;
        uint32_t rate;
        /* only the static PT numbers (0/8) are accepted for G.711 */
        if (r->fmts[i] == pt && fmt_enc(r, pt, &enc, &rate) && want &&
            sip_str_ieq_c(sip_str_c(enc), want) && rate == 8000) {
            return true;
        }
    }
    return false;
}

static bool local_supports(const uint8_t *prefs, int n, uint8_t pt)
{
    for (int i = 0; i < n; i++) {
        if (prefs[i] == pt) {
            return true;
        }
    }
    return false;
}

int sip_sdp_negotiate(const sip_sdp_t *r, const uint8_t *prefs, int n_prefs, bool want_dtmf,
                      bool answerer, sip_sdp_neg_t *out)
{
    memset(out, 0, sizeof(*out));
    out->dtmf_pt = -1;
    if (r->audio < 0) {
        return -1;
    }
    int chosen = -1;
    if (answerer) {
        for (int i = 0; i < n_prefs && chosen < 0; i++) {
            if (static_enc(prefs[i]) && remote_supports(r, prefs[i])) {
                chosen = prefs[i];
            }
        }
    } else {
        for (int i = 0; i < r->n_fmts && chosen < 0; i++) {
            uint8_t pt = r->fmts[i];
            if (local_supports(prefs, n_prefs, pt) && remote_supports(r, pt)) {
                chosen = pt;
            }
        }
    }
    if (chosen < 0) {
        return -1;
    }
    out->pt = (uint8_t)chosen;
    if (want_dtmf) {
        for (int i = 0; i < r->n_fmts; i++) {
            const char *enc;
            uint32_t rate;
            if (r->fmts[i] >= 96 && fmt_enc(r, r->fmts[i], &enc, &rate) &&
                sip_str_ieq_c(sip_str_c(enc), "telephone-event") && rate == 8000) {
                out->dtmf_pt = r->fmts[i];
                break;
            }
        }
    }
    out->ptime = r->ptime;
    out->ip = r->audio_ip;
    out->port = r->audio_port;
    out->remote_dir = r->dir;
    if (r->hold_ip) {
        /* c=0.0.0.0: the remote does not want to receive (RFC 3264 §8.4) */
        unsigned b = dir_bits(r->dir) & ~2u;
        out->remote_dir = bits_dir(b);
    }
    return 0;
}

static void build_audio(sip_wbuf_t *w, const sip_sdp_local_t *l)
{
    sip_w_fmt(w, "m=audio %u RTP/AVP", (unsigned)l->port);
    for (int i = 0; i < l->n_pts; i++) {
        sip_w_fmt(w, " %u", (unsigned)l->pts[i]);
    }
    if (l->dtmf_pt >= 0) {
        sip_w_fmt(w, " %d", l->dtmf_pt);
    }
    sip_w_cstr(w, "\r\n");
    for (int i = 0; i < l->n_pts; i++) {
        const char *enc = static_enc(l->pts[i]);
        if (enc) {
            sip_w_fmt(w, "a=rtpmap:%u %s/8000\r\n", (unsigned)l->pts[i], enc);
        }
    }
    if (l->dtmf_pt >= 0) {
        sip_w_fmt(w, "a=rtpmap:%d telephone-event/8000\r\na=fmtp:%d 0-16\r\n", l->dtmf_pt, l->dtmf_pt);
    }
    sip_w_fmt(w, "a=ptime:%u\r\n", (unsigned)l->ptime);
    sip_w_fmt(w, "a=%s\r\n", sip_sdp_dir_name(l->dir));
}

int sip_sdp_build(char *out, size_t cap, const sip_sdp_local_t *l)
{
    sip_wbuf_t w;
    char ip[16];
    sip_ip_format(l->ip, ip);
    sip_w_init(&w, out, cap);
    sip_w_fmt(&w, "v=0\r\no=sipwire %lu %lu IN IP4 %s\r\ns=sipwire\r\nc=IN IP4 %s\r\nt=0 0\r\n",
              (unsigned long)l->sess_id, (unsigned long)l->sess_ver, ip, ip);
    if (l->offer) {
        for (int i = 0; i < l->offer->n_m; i++) {
            const sip_sdp_mline_t *m = &l->offer->m[i];
            if (i == l->offer->audio) {
                build_audio(&w, l);
            } else {
                /* reject other streams by mirroring them with port 0 (§6) */
                sip_w_fmt(&w, "m=%s 0 %s %s\r\n", m->media, m->proto, m->fmt);
            }
        }
    } else {
        build_audio(&w, l);
    }
    return w.overflow ? -1 : (int)w.len;
}
