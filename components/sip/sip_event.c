/* Event package helpers: Subscription-State / Event headers, dialog-info+xml
 * (RFC 4235) and simple-message-summary (RFC 3842). See sip_event.h. */

#include "sip_event.h"

#include <string.h>

#define MSGSUM_MAX_LEN  4096
#define COUNT_CAP       999999u

static bool is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static bool is_digit(char c)
{
    return c >= '0' && c <= '9';
}

bool sip_content_type_is(sip_str_t ct, const char *type)
{
    if (!ct.p) {
        return false;
    }
    const char *semi = memchr(ct.p, ';', ct.len);
    if (semi) {
        ct.len = (size_t)(semi - ct.p);
    }
    return sip_str_ieq_c(sip_str_trim(ct), type);
}

/* ------------------------------------------------------------------ */
/* headers                                                              */

int sip_event_parse(sip_str_t v, sip_str_t *package, sip_str_t *id)
{
    v = sip_str_trim(v);
    if (!v.p || v.len == 0) {
        return -1;
    }
    const char *semi = memchr(v.p, ';', v.len);
    sip_str_t pkg = { v.p, semi ? (size_t)(semi - v.p) : v.len };
    pkg = sip_str_trim(pkg);
    if (pkg.len == 0) {
        return -1;
    }
    for (size_t i = 0; i < pkg.len; i++) {
        if (!sip_is_token_char(pkg.p[i])) {
            return -1;
        }
    }
    if (package) {
        *package = pkg;
    }
    if (id) {
        *id = SIP_STR_NULL;
        if (semi) {
            sip_str_t params = { semi, (size_t)(v.p + v.len - semi) };
            sip_param_find(params, "id", id);
        }
    }
    return 0;
}

static int param_int(sip_str_t params, const char *name)
{
    sip_str_t v;
    uint32_t x;
    if (!sip_param_find(params, name, &v) || !sip_str_to_u32(sip_str_trim(v), 0x7fffffff, &x)) {
        return -1;
    }
    return (int)x;
}

int sip_sub_state_parse(sip_str_t v, sip_sub_state_t *out)
{
    memset(out, 0, sizeof(*out));
    out->expires = -1;
    out->retry_after = -1;
    v = sip_str_trim(v);
    if (!v.p || v.len == 0) {
        return -1;
    }
    const char *semi = memchr(v.p, ';', v.len);
    sip_str_t st = sip_str_trim((sip_str_t){ v.p, semi ? (size_t)(semi - v.p) : v.len });
    if (st.len == 0) {
        return -1;
    }
    if (sip_str_ieq_c(st, "active")) {
        out->state = SIP_SUBST_ACTIVE;
    } else if (sip_str_ieq_c(st, "pending")) {
        out->state = SIP_SUBST_PENDING;
    } else if (sip_str_ieq_c(st, "terminated")) {
        out->state = SIP_SUBST_TERMINATED;
    } else {
        out->state = SIP_SUBST_OTHER;
    }
    if (!semi) {
        return 0;
    }
    sip_str_t params = { semi, (size_t)(v.p + v.len - semi) };
    out->expires = param_int(params, "expires");
    out->retry_after = param_int(params, "retry-after");
    sip_str_t r;
    if (sip_param_find(params, "reason", &r)) {
        static const struct {
            const char *name;
            sip_subreason_t reason;
        } reasons[] = {
            { "deactivated", SIP_SUBR_DEACTIVATED }, { "probation", SIP_SUBR_PROBATION },
            { "rejected", SIP_SUBR_REJECTED },       { "timeout", SIP_SUBR_TIMEOUT },
            { "giveup", SIP_SUBR_GIVEUP },           { "noresource", SIP_SUBR_NORESOURCE },
            { "invariant", SIP_SUBR_INVARIANT },
        };
        out->reason = SIP_SUBR_OTHER;
        for (size_t i = 0; i < sizeof(reasons) / sizeof(reasons[0]); i++) {
            if (sip_str_ieq_c(sip_str_trim(r), reasons[i].name)) {
                out->reason = reasons[i].reason;
                break;
            }
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* dialog-info+xml                                                      */

typedef enum {
    EL_OTHER,
    EL_DLGINFO,
    EL_DIALOG,
    EL_STATE,
} el_kind_t;

static bool xml_name_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || is_digit(c) || c == '-' || c == '_' ||
           c == '.' || c == ':';
}

/* local part of a (possibly prefixed) name: "dinfo:dialog" -> "dialog" */
static sip_str_t local_name(sip_str_t n)
{
    for (size_t i = n.len; i > 0; i--) {
        if (n.p[i - 1] == ':') {
            return (sip_str_t){ n.p + i, n.len - i };
        }
    }
    return n;
}

static const char *find_seq(const char *p, const char *e, const char *seq)
{
    size_t n = strlen(seq);
    while ((size_t)(e - p) >= n) {
        if (memcmp(p, seq, n) == 0) {
            return p;
        }
        p++;
    }
    return NULL;
}

static sip_dlg_state_t dlg_state_from(sip_str_t t)
{
    t = (sip_str_t){ t.p, t.len };
    while (t.len && is_space(t.p[0])) {
        t.p++;
        t.len--;
    }
    while (t.len && is_space(t.p[t.len - 1])) {
        t.len--;
    }
    if (sip_str_ieq_c(t, "trying")) {
        return SIP_DLG_TRYING;
    }
    if (sip_str_ieq_c(t, "proceeding")) {
        return SIP_DLG_PROCEEDING;
    }
    if (sip_str_ieq_c(t, "early")) {
        return SIP_DLG_EARLY;
    }
    if (sip_str_ieq_c(t, "confirmed")) {
        return SIP_DLG_CONFIRMED;
    }
    if (sip_str_ieq_c(t, "terminated")) {
        return SIP_DLG_TERMINATED;
    }
    return SIP_DLG_UNKNOWN;
}

int sip_dlginfo_parse(const char *xml, size_t len, sip_dlginfo_t *out)
{
    struct {
        sip_str_t name;
        el_kind_t kind;
    } stack[SIP_DLGINFO_MAX_DEPTH];
    int depth = 0;
    bool root_done = false;
    bool in_dialog = false;
    sip_dlginfo_dialog_t cur;
    const char *state_text = NULL;     /* start of the current <state> text */

    memset(out, 0, sizeof(*out));
    out->full = true;
    if (!xml || len == 0 || len > SIP_DLGINFO_MAX_LEN) {
        return -1;
    }
    const char *p = xml;
    const char *e = xml + len;
    if (len >= 3 && memcmp(p, "\xEF\xBB\xBF", 3) == 0) {
        p += 3; /* UTF-8 BOM */
    }
    memset(&cur, 0, sizeof(cur));

    while (p < e) {
        if (*p != '<') {
            if (*p == '\0' || ((depth == 0) && !is_space(*p))) {
                return -1; /* text outside the root element */
            }
            p++;
            continue;
        }
        if (e - p < 2) {
            return -1;
        }
        if (p[1] == '?') {
            const char *q = find_seq(p + 2, e, "?>");
            if (!q) {
                return -1;
            }
            p = q + 2;
            continue;
        }
        if (p[1] == '!') {
            if (e - p < 4 || memcmp(p, "<!--", 4) != 0) {
                return -1; /* DOCTYPE / CDATA: not expected here, refuse */
            }
            const char *q = find_seq(p + 4, e, "-->");
            if (!q) {
                return -1;
            }
            p = q + 3;
            continue;
        }
        const char *tag = p;
        bool end_tag = p[1] == '/';
        const char *q = p + (end_tag ? 2 : 1);
        const char *ns = q;
        while (q < e && xml_name_char(*q)) {
            q++;
        }
        sip_str_t name = { ns, (size_t)(q - ns) };
        if (name.len == 0) {
            return -1;
        }
        if (end_tag) {
            while (q < e && is_space(*q)) {
                q++;
            }
            if (q >= e || *q != '>' || depth == 0) {
                return -1;
            }
            depth--;
            if (!sip_str_eq(name, stack[depth].name)) {
                return -1; /* mismatched end tag */
            }
            switch (stack[depth].kind) {
            case EL_STATE:
                cur.state = dlg_state_from((sip_str_t){ state_text, (size_t)(tag - state_text) });
                state_text = NULL;
                break;
            case EL_DIALOG:
                if (out->n < SIP_DLGINFO_MAX_DIALOGS) {
                    out->d[out->n++] = cur;
                }
                out->n_total++;
                in_dialog = false;
                break;
            case EL_DLGINFO:
                root_done = true;
                break;
            case EL_OTHER:
                break;
            }
            p = q + 1;
            continue;
        }

        /* start tag */
        if (root_done) {
            return -1; /* a second root element */
        }
        el_kind_t parent = depth > 0 ? stack[depth - 1].kind : EL_OTHER;
        sip_str_t ln = local_name(name);
        el_kind_t kind = EL_OTHER;
        if (depth == 0) {
            if (!sip_str_eq(ln, sip_str_c("dialog-info"))) {
                return -1;
            }
            kind = EL_DLGINFO;
        } else if (parent == EL_DLGINFO && sip_str_eq(ln, sip_str_c("dialog"))) {
            kind = EL_DIALOG;
            memset(&cur, 0, sizeof(cur));
            cur.state = SIP_DLG_UNKNOWN;
            in_dialog = true;
        } else if (parent == EL_DIALOG && in_dialog && sip_str_eq(ln, sip_str_c("state"))) {
            kind = EL_STATE;
        }

        /* attributes */
        bool self_close = false;
        for (;;) {
            const char *ws = q;
            while (q < e && is_space(*q)) {
                q++;
            }
            if (q >= e) {
                return -1;
            }
            if (*q == '>') {
                q++;
                break;
            }
            if (*q == '/') {
                if (q + 1 >= e || q[1] != '>') {
                    return -1;
                }
                self_close = true;
                q += 2;
                break;
            }
            if (q == ws) {
                return -1; /* attributes need white space before them */
            }
            const char *an = q;
            while (q < e && xml_name_char(*q)) {
                q++;
            }
            sip_str_t aname = { an, (size_t)(q - an) };
            if (aname.len == 0) {
                return -1;
            }
            while (q < e && is_space(*q)) {
                q++;
            }
            if (q >= e || *q != '=') {
                return -1;
            }
            q++;
            while (q < e && is_space(*q)) {
                q++;
            }
            if (q >= e || (*q != '"' && *q != '\'')) {
                return -1;
            }
            char quote = *q++;
            const char *vs = q;
            while (q < e && *q != quote) {
                if (*q == '<' || *q == '\0') {
                    return -1;
                }
                q++;
            }
            if (q >= e) {
                return -1;
            }
            sip_str_t val = { vs, (size_t)(q - vs) };
            q++;
            sip_str_t al = local_name(aname);
            if (kind == EL_DLGINFO && sip_str_eq(aname, sip_str_c("version"))) {
                uint32_t v;
                if (!sip_str_to_u32(sip_str_trim(val), 0xffffffffu, &v)) {
                    return -1;
                }
                out->version = v;
            } else if (kind == EL_DLGINFO && sip_str_eq(aname, sip_str_c("state"))) {
                if (sip_str_eq(val, sip_str_c("full"))) {
                    out->full = true;
                } else if (sip_str_eq(val, sip_str_c("partial"))) {
                    out->full = false;
                } else {
                    return -1;
                }
            } else if (kind == EL_DIALOG && sip_str_eq(al, sip_str_c("id"))) {
                size_t n = val.len < sizeof(cur.id) - 1 ? val.len : sizeof(cur.id) - 1;
                memcpy(cur.id, val.p, n);
                cur.id[n] = '\0';
            } else if (kind == EL_DIALOG && sip_str_eq(al, sip_str_c("direction"))) {
                if (sip_str_eq(val, sip_str_c("initiator"))) {
                    cur.dir = SIP_DLGDIR_INITIATOR;
                } else if (sip_str_eq(val, sip_str_c("recipient"))) {
                    cur.dir = SIP_DLGDIR_RECIPIENT;
                }
            }
        }
        if (self_close) {
            if (kind == EL_DIALOG) {
                if (out->n < SIP_DLGINFO_MAX_DIALOGS) {
                    out->d[out->n++] = cur;
                }
                out->n_total++;
                in_dialog = false;
            } else if (kind == EL_DLGINFO) {
                root_done = true;
            }
        } else {
            if (depth >= SIP_DLGINFO_MAX_DEPTH) {
                return -1;
            }
            stack[depth].name = name;
            stack[depth].kind = kind;
            depth++;
            if (kind == EL_STATE) {
                state_text = q;
            }
        }
        p = q;
    }
    return (root_done && depth == 0) ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* simple-message-summary                                               */

/* Parse a decimal count at *p (capped), advancing *p. False if no digit. */
static bool count_at(const char **p, const char *e, uint32_t *out)
{
    const char *q = *p;
    uint32_t v = 0;
    if (q >= e || !is_digit(*q)) {
        return false;
    }
    while (q < e && is_digit(*q)) {
        if (v <= COUNT_CAP) {
            v = v * 10 + (uint32_t)(*q - '0');
        }
        q++;
    }
    *out = v > COUNT_CAP ? COUNT_CAP : v;
    *p = q;
    return true;
}

static void skip_ws(const char **p, const char *e)
{
    while (*p < e && (**p == ' ' || **p == '\t')) {
        (*p)++;
    }
}

/* "new/old" with optional "(urgent_new/urgent_old)" */
static bool parse_counts(sip_str_t v, sip_msgsum_t *out)
{
    const char *p = v.p;
    const char *e = v.p + v.len;
    uint32_t n, o, un = 0, uo = 0;
    skip_ws(&p, e);
    if (!count_at(&p, e, &n)) {
        return false;
    }
    skip_ws(&p, e);
    if (p >= e || *p != '/') {
        return false;
    }
    p++;
    skip_ws(&p, e);
    if (!count_at(&p, e, &o)) {
        return false;
    }
    skip_ws(&p, e);
    if (p < e && *p == '(') {
        p++;
        skip_ws(&p, e);
        if (!count_at(&p, e, &un)) {
            return false;
        }
        skip_ws(&p, e);
        if (p >= e || *p != '/') {
            return false;
        }
        p++;
        skip_ws(&p, e);
        if (!count_at(&p, e, &uo)) {
            return false;
        }
        skip_ws(&p, e);
        if (p >= e || *p != ')') {
            return false;
        }
        p++;
        skip_ws(&p, e);
    }
    if (p != e) {
        return false;
    }
    out->new_msgs = n;
    out->old_msgs = o;
    out->new_urgent = un;
    out->old_urgent = uo;
    out->have_voice = true;
    return true;
}

int sip_msgsum_parse(const char *body, size_t len, sip_msgsum_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!body || len == 0 || len > MSGSUM_MAX_LEN) {
        return -1;
    }
    bool have_mw = false;
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
        if (memchr(line.p, '\0', line.len)) {
            return -1;
        }
        const char *colon = memchr(line.p, ':', line.len);
        if (!colon) {
            continue; /* blank line before optional message headers, junk */
        }
        sip_str_t name = sip_str_trim((sip_str_t){ line.p, (size_t)(colon - line.p) });
        sip_str_t val = sip_str_trim((sip_str_t){ colon + 1, (size_t)(line.p + line.len - colon - 1) });
        if (sip_str_ieq_c(name, "Messages-Waiting")) {
            if (have_mw) {
                continue; /* first one wins */
            }
            if (sip_str_ieq_c(val, "yes")) {
                out->waiting = true;
            } else if (sip_str_ieq_c(val, "no")) {
                out->waiting = false;
            } else {
                return -1;
            }
            have_mw = true;
        } else if (sip_str_ieq_c(name, "Voice-Message") && !out->have_voice) {
            sip_msgsum_t tmp = *out;
            if (parse_counts(val, &tmp)) {
                *out = tmp;
            }
        }
    }
    return have_mw ? 0 : -1;
}
