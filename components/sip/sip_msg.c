/* SIP message parser and writer. See sip_msg.h. */

#include "sip_msg.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* character classes                                                    */

static bool is_ws(char c) { return c == ' ' || c == '\t'; }
static bool is_digit(char c) { return c >= '0' && c <= '9'; }
static bool is_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

/* token = 1*(alphanum / "-" / "." / "!" / "%" / "*" / "_" / "+" / "`" / "'" / "~") */
bool sip_is_token_char(char c)
{
    return is_alpha(c) || is_digit(c) || (c != '\0' && strchr("-.!%*_+`'~", c) != NULL);
}

/* host characters (hostname / IPv4); IPv6 references are handled separately */
static bool is_host_char(char c)
{
    return is_alpha(c) || is_digit(c) || c == '-' || c == '.';
}

/* ------------------------------------------------------------------ */
/* string helpers                                                       */

sip_str_t sip_str_c(const char *s)
{
    return (sip_str_t){ s, s ? strlen(s) : 0 };
}

sip_str_t sip_str_trim(sip_str_t s)
{
    if (!s.p) {
        return s;
    }
    while (s.len && is_ws(s.p[0])) {
        s.p++;
        s.len--;
    }
    while (s.len && is_ws(s.p[s.len - 1])) {
        s.len--;
    }
    return s;
}

bool sip_str_eq(sip_str_t a, sip_str_t b)
{
    return a.len == b.len && (a.len == 0 || memcmp(a.p, b.p, a.len) == 0);
}

bool sip_str_ieq(sip_str_t a, sip_str_t b)
{
    if (a.len != b.len) {
        return false;
    }
    for (size_t i = 0; i < a.len; i++) {
        if (lower(a.p[i]) != lower(b.p[i])) {
            return false;
        }
    }
    return true;
}

bool sip_str_ieq_c(sip_str_t a, const char *b)
{
    return sip_str_ieq(a, sip_str_c(b));
}

int sip_str_copy(char *dst, size_t cap, sip_str_t s)
{
    if (cap == 0) {
        return -1;
    }
    if (s.len >= cap) {
        dst[0] = '\0';
        return -1;
    }
    if (s.len) {
        memcpy(dst, s.p, s.len);
    }
    dst[s.len] = '\0';
    return (int)s.len;
}

bool sip_str_to_u32(sip_str_t s, uint32_t max, uint32_t *out)
{
    if (s.len == 0 || s.len > 10) {
        return false;
    }
    uint64_t v = 0;
    for (size_t i = 0; i < s.len; i++) {
        if (!is_digit(s.p[i])) {
            return false;
        }
        v = v * 10 + (uint64_t)(s.p[i] - '0');
    }
    if (v > max) {
        return false;
    }
    *out = (uint32_t)v;
    return true;
}

/* Skip a quoted-string starting at *p (which points at the opening quote).
 * Returns pointer after the closing quote, or NULL if unterminated. */
static const char *skip_quoted(const char *p, const char *e)
{
    p++;
    while (p < e) {
        if (*p == '\\') {
            if (p + 1 >= e) {
                return NULL;
            }
            p += 2;
            continue;
        }
        if (*p == '"') {
            return p + 1;
        }
        p++;
    }
    return NULL;
}

bool sip_list_next(sip_str_t *rest, sip_str_t *item, bool *err)
{
    const char *p = rest->p;
    const char *e = rest->p ? rest->p + rest->len : NULL;
    if (!p) {
        return false;
    }
    /* skip empty elements and whitespace */
    while (p < e && (is_ws(*p) || *p == ',')) {
        p++;
    }
    if (p >= e) {
        rest->p = e;
        rest->len = 0;
        return false;
    }
    const char *start = p;
    int angle = 0;
    while (p < e) {
        if (*p == '"') {
            p = skip_quoted(p, e);
            if (!p) {
                *err = true;
                return false;
            }
            continue;
        }
        if (*p == '<') {
            angle++;
        } else if (*p == '>') {
            if (--angle < 0) {
                *err = true;
                return false;
            }
        } else if (*p == ',' && angle == 0) {
            break;
        }
        p++;
    }
    if (angle != 0) {
        *err = true;
        return false;
    }
    *item = sip_str_trim((sip_str_t){ start, (size_t)(p - start) });
    rest->p = p;
    rest->len = (size_t)(e - p);
    return true;
}

/* Iterate ";name[=value]" pairs. Value may be a quoted-string (returned
 * without quotes) or a run of non-separator characters. Returns 1 for a
 * parameter, 0 at end, -1 malformed. stop_q: '?' ends the list (URI). */
static int param_next(const char **pp, const char *e, sip_str_t *name, sip_str_t *val, bool stop_q)
{
    const char *p = *pp;
    while (p < e && is_ws(*p)) {
        p++;
    }
    if (p >= e || (stop_q && *p == '?')) {
        *pp = p;
        return 0;
    }
    if (*p != ';') {
        return -1;
    }
    p++;
    while (p < e && is_ws(*p)) {
        p++;
    }
    const char *n0 = p;
    /* pname in URIs also allows escapes and "[]/:&+$"; be lenient but bounded */
    while (p < e && (sip_is_token_char(*p) || strchr("[]/:&+$", *p))) {
        p++;
    }
    if (p == n0) {
        return -1;
    }
    *name = (sip_str_t){ n0, (size_t)(p - n0) };
    *val = (sip_str_t){ p, 0 };
    const char *q = p;
    while (q < e && is_ws(*q)) {
        q++;
    }
    if (q < e && *q == '=') {
        q++;
        while (q < e && is_ws(*q)) {
            q++;
        }
        if (q < e && *q == '"') {
            const char *end = skip_quoted(q, e);
            if (!end) {
                return -1;
            }
            *val = (sip_str_t){ q + 1, (size_t)(end - q - 2) };
            p = end;
        } else {
            const char *v0 = q;
            while (q < e && !is_ws(*q) && *q != ';' && *q != ',' && *q != '?' && *q != '"' &&
                   *q != '<' && *q != '>') {
                q++;
            }
            if (q == v0) {
                return -1;
            }
            *val = (sip_str_t){ v0, (size_t)(q - v0) };
            p = q;
        }
    }
    *pp = p;
    return 1;
}

static bool params_valid(sip_str_t params, bool stop_q)
{
    const char *p = params.p;
    const char *e = params.p ? params.p + params.len : NULL;
    sip_str_t n, v;
    int r;
    if (!p) {
        return true;
    }
    while ((r = param_next(&p, e, &n, &v, stop_q)) == 1) {
    }
    return r == 0;
}

bool sip_param_find(sip_str_t params, const char *name, sip_str_t *val)
{
    const char *p = params.p;
    const char *e = params.p ? params.p + params.len : NULL;
    sip_str_t n, v;
    if (!p) {
        return false;
    }
    while (param_next(&p, e, &n, &v, true) == 1) {
        if (sip_str_ieq_c(n, name)) {
            if (val) {
                *val = v;
            }
            return true;
        }
    }
    return false;
}

static int hexval(char c)
{
    if (is_digit(c)) {
        return c - '0';
    }
    c = lower(c);
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

int sip_unescape(char *dst, size_t cap, sip_str_t s)
{
    size_t o = 0;
    for (size_t i = 0; i < s.len; i++) {
        char c = s.p[i];
        if (c == '%') {
            if (i + 2 >= s.len) {
                return -1; /* need two hex digits */
            }
            int h = hexval(s.p[i + 1]);
            int l = hexval(s.p[i + 2]);
            if (h < 0 || l < 0) {
                return -1;
            }
            c = (char)(h * 16 + l);
            if (c == '\0') {
                return -1;
            }
            i += 2;
        }
        if (o + 1 >= cap) {
            return -1;
        }
        dst[o++] = c;
    }
    if (cap == 0) {
        return -1;
    }
    dst[o] = '\0';
    return (int)o;
}

/* ------------------------------------------------------------------ */
/* methods / reasons                                                    */

static const char *const method_names[] = {
    [SIP_METHOD_UNKNOWN] = "",
    [SIP_METHOD_INVITE] = "INVITE",
    [SIP_METHOD_ACK] = "ACK",
    [SIP_METHOD_BYE] = "BYE",
    [SIP_METHOD_CANCEL] = "CANCEL",
    [SIP_METHOD_OPTIONS] = "OPTIONS",
    [SIP_METHOD_REGISTER] = "REGISTER",
    [SIP_METHOD_INFO] = "INFO",
    [SIP_METHOD_UPDATE] = "UPDATE",
    [SIP_METHOD_PRACK] = "PRACK",
    [SIP_METHOD_SUBSCRIBE] = "SUBSCRIBE",
    [SIP_METHOD_NOTIFY] = "NOTIFY",
    [SIP_METHOD_REFER] = "REFER",
    [SIP_METHOD_MESSAGE] = "MESSAGE",
    [SIP_METHOD_PUBLISH] = "PUBLISH",
};

sip_method_t sip_method_from_str(sip_str_t s)
{
    /* method names are case-sensitive (§7.1) */
    for (size_t i = 1; i < sizeof(method_names) / sizeof(method_names[0]); i++) {
        if (sip_str_eq(s, sip_str_c(method_names[i]))) {
            return (sip_method_t)i;
        }
    }
    return SIP_METHOD_UNKNOWN;
}

const char *sip_method_name(sip_method_t m)
{
    if ((size_t)m < sizeof(method_names) / sizeof(method_names[0])) {
        return method_names[m];
    }
    return "";
}

const char *sip_reason_phrase(int code)
{
    switch (code) {
    case 100: return "Trying";
    case 180: return "Ringing";
    case 183: return "Session Progress";
    case 200: return "OK";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 407: return "Proxy Authentication Required";
    case 408: return "Request Timeout";
    case 415: return "Unsupported Media Type";
    case 416: return "Unsupported URI Scheme";
    case 420: return "Bad Extension";
    case 423: return "Interval Too Brief";
    case 480: return "Temporarily Unavailable";
    case 481: return "Call/Transaction Does Not Exist";
    case 486: return "Busy Here";
    case 487: return "Request Terminated";
    case 488: return "Not Acceptable Here";
    case 491: return "Request Pending";
    case 500: return "Server Internal Error";
    case 501: return "Not Implemented";
    case 503: return "Service Unavailable";
    case 603: return "Decline";
    default: break;
    }
    if (code >= 200 && code < 300) return "OK";
    if (code >= 400 && code < 500) return "Client Error";
    if (code >= 500 && code < 600) return "Server Error";
    if (code >= 600) return "Global Failure";
    return "Unknown";
}

/* ------------------------------------------------------------------ */
/* URI / name-addr / Via                                                */

int sip_uri_parse(sip_str_t s, sip_uri_t *u)
{
    memset(u, 0, sizeof(*u));
    s = sip_str_trim(s);
    const char *p = s.p;
    const char *e = s.p ? s.p + s.len : NULL;
    if (!p || s.len == 0) {
        return -1;
    }
    for (size_t i = 0; i < s.len; i++) {
        if (is_ws(s.p[i]) || s.p[i] == '<' || s.p[i] == '>' || s.p[i] == '"') {
            return -1;
        }
    }
    const char *colon = memchr(p, ':', s.len);
    if (!colon) {
        return -1;
    }
    u->scheme = (sip_str_t){ p, (size_t)(colon - p) };
    if (!sip_str_ieq_c(u->scheme, "sip") && !sip_str_ieq_c(u->scheme, "sips")) {
        return -1;
    }
    p = colon + 1;
    /* '@' is never unescaped outside userinfo, while the user part may hold
     * ';', '?' and '/' (user-unreserved, §25.1) */
    const char *at = memchr(p, '@', (size_t)(e - p));
    if (at) {
        const char *uend = memchr(p, ':', (size_t)(at - p)); /* user:password */
        u->user = (sip_str_t){ p, (size_t)((uend ? uend : at) - p) };
        if (u->user.len == 0) {
            return -1;
        }
        p = at + 1;
    }
    const char *h0 = p;
    if (p < e && *p == '[') {
        const char *rb = memchr(p, ']', (size_t)(e - p));
        if (!rb) {
            return -1;
        }
        p = rb + 1;
    } else {
        while (p < e && is_host_char(*p)) {
            p++;
        }
    }
    u->host = (sip_str_t){ h0, (size_t)(p - h0) };
    if (u->host.len == 0) {
        return -1;
    }
    if (p < e && *p == ':') {
        p++;
        const char *d0 = p;
        while (p < e && is_digit(*p)) {
            p++;
        }
        uint32_t port;
        if (!sip_str_to_u32((sip_str_t){ d0, (size_t)(p - d0) }, 65535, &port) || port == 0) {
            return -1;
        }
        u->port = (int)port;
    }
    const char *qm = p < e ? memchr(p, '?', (size_t)(e - p)) : NULL;
    const char *pe = qm ? qm : e;
    if (p < pe && *p != ';') {
        return -1;
    }
    u->params = (sip_str_t){ p, (size_t)(pe - p) };
    if (!params_valid(u->params, true)) {
        return -1;
    }
    if (qm) {
        u->headers = (sip_str_t){ qm + 1, (size_t)(e - qm - 1) };
    }
    u->lr = sip_param_find(u->params, "lr", NULL);
    return 0;
}

int sip_nameaddr_parse(sip_str_t s, sip_nameaddr_t *na, bool allow_star)
{
    memset(na, 0, sizeof(*na));
    na->expires = -1;
    s = sip_str_trim(s);
    na->raw = s;
    if (!s.p || s.len == 0) {
        return -1;
    }
    const char *p = s.p;
    const char *e = s.p + s.len;
    if (s.len == 1 && *p == '*') {
        if (!allow_star) {
            return -1;
        }
        na->star = true;
        return 0;
    }
    const char *lt = NULL;
    if (*p == '"') {
        const char *q = skip_quoted(p, e);
        if (!q) {
            return -1;
        }
        na->display = (sip_str_t){ p + 1, (size_t)(q - p - 2) };
        p = q;
        while (p < e && is_ws(*p)) {
            p++;
        }
        if (p >= e || *p != '<') {
            return -1;
        }
        lt = p;
    } else {
        /* name-addr if there is a '<' before any header param; display name
         * is then *(token LWS) */
        for (const char *q = p; q < e; q++) {
            if (*q == '<') {
                lt = q;
                break;
            }
            if (*q == ';' || *q == '"') {
                break;
            }
        }
        if (lt) {
            sip_str_t d = sip_str_trim((sip_str_t){ p, (size_t)(lt - p) });
            for (size_t i = 0; i < d.len; i++) {
                if (!sip_is_token_char(d.p[i]) && !is_ws(d.p[i])) {
                    return -1;
                }
            }
            na->display = d;
        }
    }
    if (lt) {
        const char *gt = memchr(lt, '>', (size_t)(e - lt));
        if (!gt) {
            return -1;
        }
        na->uri = (sip_str_t){ lt + 1, (size_t)(gt - lt - 1) };
        p = gt + 1;
    } else {
        /* addr-spec: params after ';' are header params (§20) */
        const char *q = p;
        while (q < e && *q != ';' && !is_ws(*q)) {
            q++;
        }
        na->uri = (sip_str_t){ p, (size_t)(q - p) };
        p = q;
    }
    if (na->uri.len == 0 || !memchr(na->uri.p, ':', na->uri.len)) {
        return -1;
    }
    for (size_t i = 0; i < na->uri.len; i++) {
        if (is_ws(na->uri.p[i]) || na->uri.p[i] == '<') {
            return -1;
        }
    }
    na->params = sip_str_trim((sip_str_t){ p, (size_t)(e - p) });
    if (!params_valid(na->params, false)) {
        return -1;
    }
    sip_str_t v;
    if (sip_param_find(na->params, "tag", &na->tag) && na->tag.len == 0) {
        return -1;
    }
    if (sip_param_find(na->params, "expires", &v)) {
        uint32_t ex;
        if (!sip_str_to_u32(v, 0x7fffffff, &ex)) {
            return -1;
        }
        na->expires = (int)ex;
    }
    return 0;
}

static int via_parse(sip_str_t s, sip_via_t *v)
{
    memset(v, 0, sizeof(*v));
    v->rport = -1;
    s = sip_str_trim(s);
    v->raw = s;
    const char *p = s.p;
    const char *e = s.p + s.len;
    sip_str_t tok[3];
    /* sent-protocol = protocol-name SLASH protocol-version SLASH transport,
     * SLASH = SWS "/" SWS */
    for (int i = 0; i < 3; i++) {
        while (p < e && is_ws(*p)) {
            p++;
        }
        const char *t0 = p;
        while (p < e && sip_is_token_char(*p)) {
            p++;
        }
        tok[i] = (sip_str_t){ t0, (size_t)(p - t0) };
        if (tok[i].len == 0) {
            return -1;
        }
        while (p < e && is_ws(*p)) {
            p++;
        }
        if (i < 2) {
            if (p >= e || *p != '/') {
                return -1;
            }
            p++;
        }
    }
    if (!sip_str_ieq_c(tok[0], "SIP") || !sip_str_eq(tok[1], sip_str_c("2.0"))) {
        return -1;
    }
    v->transport = tok[2];
    const char *h0 = p;
    if (p < e && *p == '[') {
        const char *rb = memchr(p, ']', (size_t)(e - p));
        if (!rb) {
            return -1;
        }
        p = rb + 1;
    } else {
        while (p < e && is_host_char(*p)) {
            p++;
        }
    }
    v->host = (sip_str_t){ h0, (size_t)(p - h0) };
    if (v->host.len == 0) {
        return -1;
    }
    while (p < e && is_ws(*p)) {
        p++;
    }
    if (p < e && *p == ':') {
        p++;
        while (p < e && is_ws(*p)) {
            p++;
        }
        const char *d0 = p;
        while (p < e && is_digit(*p)) {
            p++;
        }
        uint32_t port;
        if (!sip_str_to_u32((sip_str_t){ d0, (size_t)(p - d0) }, 65535, &port) || port == 0) {
            return -1;
        }
        v->port = (int)port;
    }
    v->params = (sip_str_t){ p, (size_t)(e - p) };
    if (!params_valid(v->params, false)) {
        return -1;
    }
    sip_str_t val;
    if (sip_param_find(v->params, "branch", &val)) {
        if (val.len == 0) {
            return -1;
        }
        v->branch = val;
    }
    if (sip_param_find(v->params, "received", &val)) {
        v->received = val;
    }
    if (sip_param_find(v->params, "rport", &val)) {
        uint32_t rp = 0;
        if (val.len && (!sip_str_to_u32(val, 65535, &rp) || rp == 0)) {
            return -1;
        }
        v->rport = (int)rp;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* message                                                              */

typedef enum {
    H_UNKNOWN,
    H_VIA, H_FROM, H_TO, H_CALL_ID, H_CSEQ, H_CONTACT, H_ROUTE, H_RECORD_ROUTE,
    H_CONTENT_LENGTH, H_CONTENT_TYPE, H_WWW_AUTH, H_PROXY_AUTH, H_EXPIRES,
    H_MIN_EXPIRES, H_MAX_FORWARDS, H_ALLOW, H_SUPPORTED, H_REQUIRE, H_USER_AGENT,
    H_EVENT, H_SUB_STATE, H_RETRY_AFTER, H_CALL_INFO, H_ALERT_INFO,
} hdr_id_t;

static const struct {
    const char *name;
    const char *compact;
    hdr_id_t id;
} hdr_table[] = {
    { "Via", "v", H_VIA },
    { "From", "f", H_FROM },
    { "To", "t", H_TO },
    { "Call-ID", "i", H_CALL_ID },
    { "CSeq", NULL, H_CSEQ },
    { "Contact", "m", H_CONTACT },
    { "Route", NULL, H_ROUTE },
    { "Record-Route", NULL, H_RECORD_ROUTE },
    { "Content-Length", "l", H_CONTENT_LENGTH },
    { "Content-Type", "c", H_CONTENT_TYPE },
    { "WWW-Authenticate", NULL, H_WWW_AUTH },
    { "Proxy-Authenticate", NULL, H_PROXY_AUTH },
    { "Expires", NULL, H_EXPIRES },
    { "Min-Expires", NULL, H_MIN_EXPIRES },
    { "Max-Forwards", NULL, H_MAX_FORWARDS },
    { "Allow", NULL, H_ALLOW },
    { "Supported", "k", H_SUPPORTED },
    { "Require", NULL, H_REQUIRE },
    { "User-Agent", NULL, H_USER_AGENT },
    { "Event", "o", H_EVENT },
    { "Subscription-State", NULL, H_SUB_STATE },
    { "Retry-After", NULL, H_RETRY_AFTER },
    { "Call-Info", NULL, H_CALL_INFO },
    { "Alert-Info", NULL, H_ALERT_INFO },
    /* "s" (Subject), "e" (Content-Encoding), "r", "u" compact forms map
     * to headers we ignore. */
};

static hdr_id_t hdr_lookup(sip_str_t name)
{
    for (size_t i = 0; i < sizeof(hdr_table) / sizeof(hdr_table[0]); i++) {
        if (sip_str_ieq_c(name, hdr_table[i].name) ||
            (hdr_table[i].compact && sip_str_ieq_c(name, hdr_table[i].compact))) {
            return hdr_table[i].id;
        }
    }
    return H_UNKNOWN;
}

static bool parse_int_hdr(sip_str_t v, uint32_t max, int *out)
{
    uint32_t x;
    if (*out != -1) {
        return false; /* duplicate single-value header */
    }
    if (!sip_str_to_u32(sip_str_trim(v), max, &x)) {
        return false;
    }
    *out = (int)x;
    return true;
}

static bool set_single(sip_str_t *dst, sip_str_t v)
{
    if (dst->p) {
        return false; /* duplicate */
    }
    *dst = v.p ? v : (sip_str_t){ "", 0 };
    return true;
}

static int parse_list_nameaddr(sip_str_t v, sip_nameaddr_t *arr, int *n, int max, bool star)
{
    bool err = false;
    sip_str_t item;
    while (sip_list_next(&v, &item, &err)) {
        if (*n >= max) {
            return -1;
        }
        if (sip_nameaddr_parse(item, &arr[*n], star) != 0) {
            return -1;
        }
        (*n)++;
    }
    return err ? -1 : 0;
}

static int header_apply(sip_msg_t *m, sip_str_t name, sip_str_t v)
{
    bool err = false;
    sip_str_t item;
    switch (hdr_lookup(name)) {
    case H_VIA:
        while (sip_list_next(&v, &item, &err)) {
            if (m->n_via >= SIP_MAX_VIA || via_parse(item, &m->via[m->n_via]) != 0) {
                return -1;
            }
            m->n_via++;
        }
        return err ? -1 : 0;
    case H_FROM:
        if (m->from.raw.p) {
            return -1;
        }
        return sip_nameaddr_parse(v, &m->from, false);
    case H_TO:
        if (m->to.raw.p) {
            return -1;
        }
        return sip_nameaddr_parse(v, &m->to, false);
    case H_CALL_ID:
        if (m->call_id.p || v.len == 0) {
            return -1;
        }
        for (size_t i = 0; i < v.len; i++) {
            if (is_ws(v.p[i]) || (unsigned char)v.p[i] < 0x21 || (unsigned char)v.p[i] > 0x7e) {
                return -1;
            }
        }
        m->call_id = v;
        return 0;
    case H_CSEQ: {
        if (m->cseq_method_str.p) {
            return -1;
        }
        const char *p = v.p;
        const char *e = v.p + v.len;
        const char *d0 = p;
        while (p < e && is_digit(*p)) {
            p++;
        }
        if (!sip_str_to_u32((sip_str_t){ d0, (size_t)(p - d0) }, SIP_MAX_CSEQ, &m->cseq)) {
            return -1;
        }
        if (p >= e || !is_ws(*p)) {
            return -1;
        }
        while (p < e && is_ws(*p)) {
            p++;
        }
        sip_str_t meth = { p, (size_t)(e - p) };
        if (meth.len == 0) {
            return -1;
        }
        for (size_t i = 0; i < meth.len; i++) {
            if (!sip_is_token_char(meth.p[i])) {
                return -1;
            }
        }
        m->cseq_method_str = meth;
        m->cseq_method = sip_method_from_str(meth);
        return 0;
    }
    case H_CONTACT:
        return parse_list_nameaddr(v, m->contact, &m->n_contact, SIP_MAX_CONTACT, true);
    case H_ROUTE:
        return parse_list_nameaddr(v, m->route, &m->n_route, SIP_MAX_ROUTE, false);
    case H_RECORD_ROUTE:
        return parse_list_nameaddr(v, m->rr, &m->n_rr, SIP_MAX_ROUTE, false);
    case H_CONTENT_LENGTH:
        return parse_int_hdr(v, 65535, &m->content_length) ? 0 : -1;
    case H_CONTENT_TYPE:
        return set_single(&m->content_type, v) ? 0 : -1;
    case H_WWW_AUTH:
        if (m->n_www_auth < SIP_MAX_AUTH) {
            m->www_auth[m->n_www_auth++] = v;
        }
        return 0;
    case H_PROXY_AUTH:
        if (m->n_proxy_auth < SIP_MAX_AUTH) {
            m->proxy_auth[m->n_proxy_auth++] = v;
        }
        return 0;
    case H_EXPIRES:
        return parse_int_hdr(v, 0x7fffffff, &m->expires) ? 0 : -1;
    case H_MIN_EXPIRES:
        return parse_int_hdr(v, 0x7fffffff, &m->min_expires) ? 0 : -1;
    case H_MAX_FORWARDS:
        return parse_int_hdr(v, 255, &m->max_forwards) ? 0 : -1;
    case H_ALLOW:
        m->allow = v; /* may repeat; last wins, informational only */
        return 0;
    case H_SUPPORTED:
        m->supported = v;
        return 0;
    case H_REQUIRE:
        return set_single(&m->require, v) ? 0 : -1;
    case H_USER_AGENT:
        m->user_agent = v;
        return 0;
    case H_EVENT:
        return set_single(&m->event, v) ? 0 : -1;
    case H_SUB_STATE:
        return set_single(&m->sub_state, v) ? 0 : -1;
    case H_RETRY_AFTER: {
        /* Retry-After: delta-seconds [comment] *(;param) -- only the number */
        sip_str_t t = sip_str_trim(v);
        size_t n = 0;
        while (n < t.len && is_digit(t.p[n])) {
            n++;
        }
        if (m->retry_after != -1) {
            return -1;
        }
        uint32_t x;
        if (n == 0 || !sip_str_to_u32((sip_str_t){ t.p, n }, 0x7fffffff, &x)) {
            return 0; /* malformed: ignore, it is only a hint */
        }
        m->retry_after = (int)x;
        return 0;
    }
    case H_CALL_INFO:
        if (m->n_call_info < SIP_MAX_INFO_HDR) {
            m->call_info[m->n_call_info++] = v;
        }
        return 0;
    case H_ALERT_INFO:
        if (m->n_alert_info < SIP_MAX_INFO_HDR) {
            m->alert_info[m->n_alert_info++] = v;
        }
        return 0;
    case H_UNKNOWN:
        return 0;
    }
    return 0;
}

static int parse_start_line(sip_msg_t *m, sip_str_t line)
{
    const char *p = line.p;
    const char *e = line.p + line.len;
    const char *sp1 = memchr(p, ' ', line.len);
    if (!sp1) {
        return -1;
    }
    sip_str_t first = { p, (size_t)(sp1 - p) };
    if (sip_str_ieq_c(first, "SIP/2.0")) {
        /* Status-Line = SIP-Version SP Status-Code SP Reason-Phrase */
        const char *c = sp1 + 1;
        if (e - c < 3) {
            return -1;
        }
        uint32_t code;
        if (!sip_str_to_u32((sip_str_t){ c, 3 }, 699, &code) || code < 100) {
            return -1;
        }
        c += 3;
        if (c < e) {
            if (*c != ' ') {
                return -1;
            }
            m->reason = (sip_str_t){ c + 1, (size_t)(e - c - 1) };
        }
        m->status = (int)code;
        m->is_request = false;
        return 0;
    }
    /* Request-Line = Method SP Request-URI SP SIP-Version (single SPs) */
    if (first.len == 0) {
        return -1;
    }
    for (size_t i = 0; i < first.len; i++) {
        if (!sip_is_token_char(first.p[i])) {
            return -1;
        }
    }
    const char *u = sp1 + 1;
    const char *sp2 = memchr(u, ' ', (size_t)(e - u));
    if (!sp2 || sp2 == u) {
        return -1;
    }
    sip_str_t ver = { sp2 + 1, (size_t)(e - sp2 - 1) };
    if (!sip_str_ieq_c(ver, "SIP/2.0")) {
        return -1;
    }
    m->is_request = true;
    m->method_str = first;
    m->method = sip_method_from_str(first);
    m->ruri = (sip_str_t){ u, (size_t)(sp2 - u) };
    for (size_t i = 0; i < m->ruri.len; i++) {
        unsigned char c = (unsigned char)m->ruri.p[i];
        if (c < 0x21 || c > 0x7e || c == '<' || c == '>' || c == '"') {
            return -1; /* RFC 4475 ltgtruri */
        }
    }
    if (!memchr(m->ruri.p, ':', m->ruri.len)) {
        return -1;
    }
    return 0;
}

sip_parse_result_t sip_msg_parse(sip_msg_t *m, char *buf, size_t len)
{
    memset(m, 0, sizeof(*m));
    m->content_length = -1;
    m->expires = -1;
    m->min_expires = -1;
    m->max_forwards = -1;
    m->retry_after = -1;
    if (!buf || len == 0) {
        return SIP_PARSE_ERR;
    }
    if (len > SIP_MAX_MSG_IN) {
        return SIP_PARSE_TOO_BIG;
    }

    /* Pass 1: locate the end of the header section and unfold continuation
     * lines (§7.3.1) by blanking the preceding line terminator. */
    size_t i = 0;
    size_t start = 0;
    size_t body_off = 0;
    size_t prev_term = 0;
    size_t prev_nl = 0;
    int line_no = 0;
    bool found_end = false;
    while (i < len) {
        const char *nl = memchr(buf + i, '\n', len - i);
        if (!nl) {
            return SIP_PARSE_ERR;
        }
        size_t nlpos = (size_t)(nl - buf);
        size_t end = nlpos;
        if (end > i && buf[end - 1] == '\r') {
            end--;
        }
        for (size_t k = i; k < end; k++) {
            if (buf[k] == '\0' || buf[k] == '\r') {
                return SIP_PARSE_ERR;
            }
        }
        if (end == i) {
            if (line_no == 0) {
                i = nlpos + 1; /* leading CRLF before the start line */
                start = i;
                continue;
            }
            body_off = nlpos + 1;
            found_end = true;
            break;
        }
        if (is_ws(buf[i])) {
            if (line_no <= 1) {
                return SIP_PARSE_ERR; /* start line cannot be folded */
            }
            for (size_t k = prev_term; k <= prev_nl; k++) {
                buf[k] = ' ';
            }
        }
        prev_term = end;
        prev_nl = nlpos;
        line_no++;
        i = nlpos + 1;
    }
    if (!found_end || line_no == 0) {
        return SIP_PARSE_ERR;
    }

    /* Pass 2: start line + headers */
    size_t hdr_end = body_off; /* lines below are all before the blank line */
    i = start;
    int n_hdr = 0;
    bool first = true;
    while (i < hdr_end) {
        const char *nl = memchr(buf + i, '\n', hdr_end - i);
        if (!nl) {
            return SIP_PARSE_ERR;
        }
        size_t nlpos = (size_t)(nl - buf);
        size_t end = nlpos;
        if (end > i && buf[end - 1] == '\r') {
            end--;
        }
        sip_str_t line = { buf + i, end - i };
        i = nlpos + 1;
        if (line.len == 0) {
            break; /* the blank separator line */
        }
        if (first) {
            first = false;
            if (parse_start_line(m, line) != 0) {
                return SIP_PARSE_ERR;
            }
            continue;
        }
        if (++n_hdr > SIP_MAX_HEADERS) {
            return SIP_PARSE_ERR;
        }
        const char *p = line.p;
        const char *e = line.p + line.len;
        while (p < e && sip_is_token_char(*p)) {
            p++;
        }
        sip_str_t name = { line.p, (size_t)(p - line.p) };
        while (p < e && is_ws(*p)) {
            p++;
        }
        if (name.len == 0 || p >= e || *p != ':') {
            return SIP_PARSE_ERR;
        }
        p++;
        sip_str_t value = sip_str_trim((sip_str_t){ p, (size_t)(e - p) });
        if (header_apply(m, name, value) != 0) {
            return SIP_PARSE_ERR;
        }
    }

    /* mandatory headers (§8.1.1) */
    if (m->n_via == 0 || !m->from.raw.p || !m->to.raw.p || !m->call_id.p || !m->cseq_method_str.p) {
        return SIP_PARSE_ERR;
    }
    if (m->is_request && !sip_str_eq(m->cseq_method_str, m->method_str)) {
        return SIP_PARSE_ERR;
    }
    /* body (§18.3 for datagrams) */
    size_t avail = len - body_off;
    if (m->content_length >= 0) {
        if ((size_t)m->content_length > avail) {
            return SIP_PARSE_ERR;
        }
        avail = (size_t)m->content_length;
    }
    m->body = (sip_str_t){ buf + body_off, avail };
    return SIP_PARSE_OK;
}

/* ------------------------------------------------------------------ */
/* writer                                                               */

void sip_w_init(sip_wbuf_t *w, char *buf, size_t cap)
{
    w->buf = buf;
    w->cap = cap;
    w->len = 0;
    w->overflow = cap == 0;
    if (cap) {
        buf[0] = '\0';
    }
}

void sip_w_mem(sip_wbuf_t *w, const void *p, size_t n)
{
    if (w->overflow) {
        return;
    }
    if (w->len + n + 1 > w->cap) {
        w->overflow = true;
        return;
    }
    if (n) {
        memcpy(w->buf + w->len, p, n);
    }
    w->len += n;
    w->buf[w->len] = '\0';
}

void sip_w_cstr(sip_wbuf_t *w, const char *s)
{
    sip_w_mem(w, s, strlen(s));
}

void sip_w_str(sip_wbuf_t *w, sip_str_t s)
{
    sip_w_mem(w, s.p, s.len);
}

void sip_w_vfmt(sip_wbuf_t *w, const char *fmt, va_list ap)
{
    if (w->overflow) {
        return;
    }
    size_t room = w->cap - w->len;
    int n = vsnprintf(w->buf + w->len, room, fmt, ap);
    if (n < 0 || (size_t)n >= room) {
        w->overflow = true;
        w->buf[w->len] = '\0';
        return;
    }
    w->len += (size_t)n;
}

void sip_w_fmt(sip_wbuf_t *w, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    sip_w_vfmt(w, fmt, ap);
    va_end(ap);
}
