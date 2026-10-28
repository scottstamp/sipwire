/* Digest authentication. See sip_auth.h. */

#include "sip_auth.h"

#include <stdio.h>
#include <string.h>

#include "sip_md5.h"

/* Copy a (possibly quoted, already de-quoted by sip_param style parsing)
 * auth-param value, resolving quoted-pair escapes. */
static int copy_unescaped(char *dst, size_t cap, sip_str_t v)
{
    size_t o = 0;
    for (size_t i = 0; i < v.len; i++) {
        char c = v.p[i];
        if (c == '\\' && i + 1 < v.len) {
            c = v.p[++i];
        }
        if (c == '\0' || o + 1 >= cap) {
            return -1;
        }
        dst[o++] = c;
    }
    dst[o] = '\0';
    return 0;
}

/* Split "name=value" (value token or quoted-string). */
static int split_auth_param(sip_str_t item, sip_str_t *name, sip_str_t *val)
{
    const char *p = item.p;
    const char *e = item.p + item.len;
    const char *n0 = p;
    while (p < e && sip_is_token_char(*p)) {
        p++;
    }
    *name = (sip_str_t){ n0, (size_t)(p - n0) };
    while (p < e && (*p == ' ' || *p == '\t')) {
        p++;
    }
    if (name->len == 0 || p >= e || *p != '=') {
        return -1;
    }
    p++;
    while (p < e && (*p == ' ' || *p == '\t')) {
        p++;
    }
    if (p < e && *p == '"') {
        const char *q = p + 1;
        while (q < e && *q != '"') {
            if (*q == '\\') {
                q++;
            }
            q++;
        }
        if (q >= e) {
            return -1;
        }
        *val = (sip_str_t){ p + 1, (size_t)(q - p - 1) };
        q++;
        while (q < e && (*q == ' ' || *q == '\t')) {
            q++;
        }
        return q == e ? 0 : -1;
    }
    *val = sip_str_trim((sip_str_t){ p, (size_t)(e - p) });
    for (size_t i = 0; i < val->len; i++) {
        if (!sip_is_token_char(val->p[i])) {
            return -1;
        }
    }
    return 0;
}

static bool list_has_token(sip_str_t list, const char *tok)
{
    const char *p = list.p;
    const char *e = list.p + list.len;
    while (p < e) {
        while (p < e && (*p == ' ' || *p == '\t' || *p == ',')) {
            p++;
        }
        const char *t0 = p;
        while (p < e && *p != ',' && *p != ' ' && *p != '\t') {
            p++;
        }
        if (p > t0 && sip_str_ieq_c((sip_str_t){ t0, (size_t)(p - t0) }, tok)) {
            return true;
        }
    }
    return false;
}

int sip_auth_parse_challenge(sip_str_t value, bool proxy, sip_auth_t *out)
{
    memset(out, 0, sizeof(*out));
    value = sip_str_trim(value);
    if (value.len < 7 || !sip_str_ieq_c((sip_str_t){ value.p, 6 }, "Digest") ||
        (value.p[6] != ' ' && value.p[6] != '\t')) {
        return -1;
    }
    sip_str_t rest = { value.p + 7, value.len - 7 };
    sip_str_t item;
    bool err = false;
    bool have_realm = false, have_nonce = false;
    while (sip_list_next(&rest, &item, &err)) {
        sip_str_t n, v;
        if (split_auth_param(item, &n, &v) != 0) {
            return -1;
        }
        if (sip_str_ieq_c(n, "realm")) {
            if (copy_unescaped(out->realm, sizeof(out->realm), v) != 0) {
                return -1;
            }
            have_realm = true;
        } else if (sip_str_ieq_c(n, "nonce")) {
            if (copy_unescaped(out->nonce, sizeof(out->nonce), v) != 0) {
                return -1;
            }
            have_nonce = true;
        } else if (sip_str_ieq_c(n, "opaque")) {
            if (copy_unescaped(out->opaque, sizeof(out->opaque), v) != 0) {
                return -1;
            }
            out->has_opaque = true;
        } else if (sip_str_ieq_c(n, "algorithm")) {
            if (!sip_str_ieq_c(v, "MD5")) {
                return -1; /* MD5-sess, SHA-256...: not supported */
            }
        } else if (sip_str_ieq_c(n, "qop")) {
            out->qop_auth = list_has_token(v, "auth");
            if (!out->qop_auth) {
                return -1; /* only auth-int offered */
            }
        } else if (sip_str_ieq_c(n, "stale")) {
            out->stale = sip_str_ieq_c(v, "true");
        }
    }
    if (err || !have_realm || !have_nonce) {
        return -1;
    }
    out->proxy = proxy;
    out->valid = true;
    return 0;
}

int sip_auth_from_response(const sip_msg_t *m, sip_auth_t *out)
{
    const sip_str_t *list = m->status == 407 ? m->proxy_auth : m->www_auth;
    int n = m->status == 407 ? m->n_proxy_auth : m->n_www_auth;
    for (int i = 0; i < n; i++) {
        if (sip_auth_parse_challenge(list[i], m->status == 407, out) == 0) {
            return 0;
        }
    }
    memset(out, 0, sizeof(*out));
    return -1;
}

static void md5_join(char out[33], const char *const *parts, int n)
{
    sip_md5_ctx_t c;
    uint8_t d[16];
    sip_md5_init(&c);
    for (int i = 0; i < n; i++) {
        if (i) {
            sip_md5_update(&c, ":", 1);
        }
        sip_md5_update(&c, parts[i], strlen(parts[i]));
    }
    sip_md5_final(&c, d);
    sip_md5_to_hex(d, out);
}

void sip_digest_response(const char *user, const char *realm, const char *pass,
                         const char *method, const char *uri, const char *nonce,
                         const char *nc, const char *cnonce, const char *qop, char out[33])
{
    char ha1[33], ha2[33];
    md5_join(ha1, (const char *const[]){ user, realm, pass }, 3);
    md5_join(ha2, (const char *const[]){ method, uri }, 2);
    if (qop) {
        md5_join(out, (const char *const[]){ ha1, nonce, nc, cnonce, qop, ha2 }, 6);
    } else {
        md5_join(out, (const char *const[]){ ha1, nonce, ha2 }, 3);
    }
    /* scrub the password-derived intermediate */
    memset(ha1, 0, sizeof(ha1));
}

static void w_quoted(sip_wbuf_t *w, const char *s)
{
    sip_w_cstr(w, "\"");
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') {
            sip_w_cstr(w, "\\");
        }
        sip_w_mem(w, s, 1);
    }
    sip_w_cstr(w, "\"");
}

void sip_auth_write_header(sip_wbuf_t *w, sip_auth_t *a, const char *user, const char *pass,
                           const char *method, const char *uri, const char *cnonce)
{
    char resp[33];
    char nc[9];
    a->nc++;
    snprintf(nc, sizeof(nc), "%08x", (unsigned)a->nc);
    sip_digest_response(user, a->realm, pass, method, uri, a->nonce,
                        nc, cnonce, a->qop_auth ? "auth" : NULL, resp);
    sip_w_cstr(w, a->proxy ? "Proxy-Authorization: Digest username=" : "Authorization: Digest username=");
    w_quoted(w, user);
    sip_w_cstr(w, ", realm=");
    w_quoted(w, a->realm);
    sip_w_cstr(w, ", nonce=");
    w_quoted(w, a->nonce);
    sip_w_cstr(w, ", uri=");
    w_quoted(w, uri);
    sip_w_cstr(w, ", response=");
    w_quoted(w, resp);
    sip_w_cstr(w, ", algorithm=MD5");
    if (a->qop_auth) {
        sip_w_cstr(w, ", qop=auth, nc=");
        sip_w_cstr(w, nc);
        sip_w_cstr(w, ", cnonce=");
        w_quoted(w, cnonce);
    }
    if (a->has_opaque) {
        sip_w_cstr(w, ", opaque=");
        w_quoted(w, a->opaque);
    }
    sip_w_cstr(w, "\r\n");
}
