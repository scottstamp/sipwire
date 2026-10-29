#include "sipwire_blf.h"

#include <string.h>

bool sipwire_blf_ext_char_ok(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '*' ||
           c == '#' || c == '+' || c == '.' || c == '_' || c == '-';
}

bool sipwire_blf_label_char_ok(char c)
{
    unsigned char u = (unsigned char)c;
    return u >= 0x20 && u != 0x7f && c != ',' && c != ':';
}

static bool is_ws(char c)
{
    return c == ' ' || c == '\t';
}

/* [p, e) trimmed */
static void trim(const char **p, const char **e)
{
    while (*p < *e && is_ws(**p)) {
        (*p)++;
    }
    while (*e > *p && is_ws((*e)[-1])) {
        (*e)--;
    }
}

int sipwire_blf_parse(const char *list, sipwire_blf_entry_t *out, int max)
{
    int n = 0;
    sipwire_blf_entry_t cur[SIPWIRE_BLF_MAX];
    if (!list) {
        return 0;
    }
    if (max > SIPWIRE_BLF_MAX) {
        max = SIPWIRE_BLF_MAX;
    }
    const char *p = list;
    while (*p) {
        const char *comma = strchr(p, ',');
        const char *end = comma ? comma : p + strlen(p);
        const char *colon = memchr(p, ':', (size_t)(end - p));
        const char *es = p, *ee = colon ? colon : end;
        const char *ls = colon ? colon + 1 : end, *le = end;
        trim(&es, &ee);
        trim(&ls, &le);
        p = comma ? comma + 1 : end;
        if (es == ee && ls == le && !colon) {
            continue; /* empty item */
        }
        size_t el = (size_t)(ee - es), ll = (size_t)(le - ls);
        if (el == 0 || el >= SIPWIRE_BLF_EXT_MAX || ll >= SIPWIRE_BLF_LABEL_MAX || n >= max) {
            return -1;
        }
        for (size_t i = 0; i < el; i++) {
            if (!sipwire_blf_ext_char_ok(es[i])) {
                return -1;
            }
        }
        for (size_t i = 0; i < ll; i++) {
            if (!sipwire_blf_label_char_ok(ls[i])) {
                return -1;
            }
        }
        memcpy(cur[n].ext, es, el);
        cur[n].ext[el] = '\0';
        memcpy(cur[n].label, ls, ll);
        cur[n].label[ll] = '\0';
        for (int k = 0; k < n; k++) {
            if (strcmp(cur[k].ext, cur[n].ext) == 0) {
                return -1;
            }
        }
        n++;
    }
    if (out) {
        memcpy(out, cur, (size_t)n * sizeof(cur[0]));
    }
    return n;
}

bool sipwire_blf_format(const sipwire_blf_entry_t *e, int n, char *out, size_t cap)
{
    size_t len = 0;
    if (cap == 0) {
        return false;
    }
    out[0] = '\0';
    for (int i = 0; i < n; i++) {
        size_t el = strlen(e[i].ext), ll = strlen(e[i].label);
        size_t need = (i ? 1 : 0) + el + (ll ? 1 + ll : 0);
        if (len + need + 1 > cap) {
            out[0] = '\0';
            return false;
        }
        if (i) {
            out[len++] = ',';
        }
        memcpy(out + len, e[i].ext, el);
        len += el;
        if (ll) {
            out[len++] = ':';
            memcpy(out + len, e[i].label, ll);
            len += ll;
        }
        out[len] = '\0';
    }
    return true;
}
