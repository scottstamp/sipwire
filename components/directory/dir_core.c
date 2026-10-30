#include "dir_core.h"

#include <ctype.h>
#include <string.h>

/* ---- normalisation ---- */

static bool starts_with_ci(const char *s, const char *prefix)
{
    for (; *prefix; s++, prefix++) {
        if (tolower((unsigned char)*s) != *prefix) {
            return false;
        }
    }
    return true;
}

void dir_core_normalize(const char *in, char *out, size_t cap)
{
    if (cap == 0) {
        return;
    }
    out[0] = '\0';
    if (in == NULL) {
        return;
    }
    while (*in == ' ' || *in == '\t') {
        in++;
    }
    /* `"Bob" <sip:1002@host>` -> inside the angle brackets */
    const char *lt = strchr(in, '<');
    const char *gt = lt ? strchr(lt, '>') : NULL;
    size_t len;
    if (lt && gt) {
        in = lt + 1;
        len = (size_t)(gt - in);
    } else {
        len = strlen(in);
    }
    char tmp[96];
    if (len >= sizeof(tmp)) {
        len = sizeof(tmp) - 1;
    }
    memcpy(tmp, in, len);
    tmp[len] = '\0';

    char *p = tmp;
    if (starts_with_ci(p, "sips:")) {
        p += 5;
    } else if (starts_with_ci(p, "sip:")) {
        p += 4;
    } else if (starts_with_ci(p, "tel:")) {
        p += 4;
    }
    char *cut = strpbrk(p, "@;?");
    if (cut) {
        *cut = '\0';
    }

    bool alpha = false;
    for (const char *q = p; *q; q++) {
        if (isalpha((unsigned char)*q)) {
            alpha = true;
        }
    }
    size_t o = 0;
    for (const char *q = p; *q && o + 1 < cap; q++) {
        unsigned char c = (unsigned char)*q;
        if (alpha) {
            if (c > ' ' && c != 0x7f) {
                out[o++] = (char)tolower(c);
            }
        } else if (isdigit(c) || c == '*' || c == '#' || (c == '+' && o == 0)) {
            out[o++] = (char)c;
        }
    }
    out[o] = '\0';
}

void dir_core_clean_name(const char *in, char *out, size_t cap)
{
    if (cap == 0) {
        return;
    }
    size_t o = 0;
    if (in) {
        for (; *in && o + 1 < cap; in++) {
            unsigned char c = (unsigned char)*in;
            if (c < ' ' || c == 0x7f) {
                c = ' ';
            }
            if (c == ' ' && (o == 0 || out[o - 1] == ' ')) {
                continue;   /* leading blanks, runs of blanks */
            }
            out[o++] = (char)c;
        }
    }
    while (o > 0 && out[o - 1] == ' ') {
        o--;
    }
    /* If the copy stopped inside a UTF-8 sequence, drop the partial sequence. */
    if (in && *in && ((unsigned char)*in & 0xC0) == 0x80) {
        size_t k = o;
        while (k > 0 && ((unsigned char)out[k - 1] & 0xC0) == 0x80) {
            k--;
        }
        if (k > 0 && (unsigned char)out[k - 1] >= 0xC0) {
            o = k - 1;
        }
    }
    out[o] = '\0';
}

/* ---- contacts table ---- */

static int strcmp_ci(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        int d = tolower((unsigned char)*a) - tolower((unsigned char)*b);
        if (d) {
            return d;
        }
    }
    return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}

static const char *strstr_ci(const char *h, const char *n)
{
    size_t nl = strlen(n);
    if (nl == 0) {
        return h;
    }
    for (; *h; h++) {
        size_t i = 0;
        while (i < nl && h[i] && tolower((unsigned char)h[i]) == tolower((unsigned char)n[i])) {
            i++;
        }
        if (i == nl) {
            return h;
        }
    }
    return NULL;
}

static int contact_cmp(const dir_contact_t *a, const dir_contact_t *b)
{
    int d = strcmp_ci(a->name, b->name);
    return d ? d : strcmp(a->number, b->number);
}

int dir_ctab_find(const dir_ctab_t *t, const char *number)
{
    char n[DIR_NUMBER_MAX];
    dir_core_normalize(number, n, sizeof(n));
    if (n[0] == '\0') {
        return -1;
    }
    for (size_t i = 0; i < t->n; i++) {
        if (strcmp(t->v[i].number, n) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static void ctab_remove_at(dir_ctab_t *t, size_t i)
{
    memmove(&t->v[i], &t->v[i + 1], (t->n - i - 1) * sizeof(t->v[0]));
    t->n--;
}

int dir_ctab_set(dir_ctab_t *t, const char *number, const char *name)
{
    dir_contact_t c;
    dir_core_normalize(number, c.number, sizeof(c.number));
    dir_core_clean_name(name, c.name, sizeof(c.name));
    if (c.number[0] == '\0' || c.name[0] == '\0') {
        return DIR_C_INVALID;
    }
    int idx = dir_ctab_find(t, c.number);
    int ret = DIR_C_UPDATED;
    if (idx >= 0) {
        if (strcmp(t->v[idx].name, c.name) == 0) {
            return DIR_C_UPDATED;
        }
        ctab_remove_at(t, (size_t)idx);
    } else if (t->n >= t->cap) {
        return DIR_C_FULL;
    } else {
        ret = DIR_C_ADDED;
    }
    size_t pos = 0;
    while (pos < t->n && contact_cmp(&t->v[pos], &c) < 0) {
        pos++;
    }
    memmove(&t->v[pos + 1], &t->v[pos], (t->n - pos) * sizeof(t->v[0]));
    t->v[pos] = c;
    t->n++;
    return ret;
}

bool dir_ctab_del(dir_ctab_t *t, const char *number)
{
    int idx = dir_ctab_find(t, number);
    if (idx < 0) {
        return false;
    }
    ctab_remove_at(t, (size_t)idx);
    return true;
}

size_t dir_ctab_search(const dir_ctab_t *t, const char *query, size_t *idx, size_t max)
{
    char nq[DIR_NUMBER_MAX];
    char q[DIR_NAME_MAX];
    dir_core_clean_name(query, q, sizeof(q));
    dir_core_normalize(q, nq, sizeof(nq));
    size_t n = 0;
    for (int pass = 0; pass < 2; pass++) {
        for (size_t i = 0; i < t->n && n < max; i++) {
            const dir_contact_t *c = &t->v[i];
            bool pre = q[0] == '\0' || starts_with_ci(c->name, q) ||
                       (nq[0] && strncmp(c->number, nq, strlen(nq)) == 0);
            bool any = pre || strstr_ci(c->name, q) || (nq[0] && strstr(c->number, nq));
            if ((pass == 0 && pre) || (pass == 1 && any && !pre)) {
                idx[n++] = i;
            }
        }
    }
    return n;
}

/* ---- CSV ---- */

static bool is_delim(char c)
{
    return c == ',' || c == ';' || c == '\t';
}

/* Reads one field at p into out; returns the position after its delimiter, or NULL when the
 * field ended the line. `last`: take the rest of the line (unquoted commas allowed in a name). */
static const char *csv_field(const char *p, char *out, size_t cap, bool last)
{
    size_t o = 0;
    while (*p == ' ') {
        p++;
    }
    if (*p == '"') {
        p++;
        while (*p) {
            if (*p == '"') {
                if (p[1] == '"') {
                    if (o + 1 < cap) {
                        out[o++] = '"';
                    }
                    p += 2;
                    continue;
                }
                p++;
                break;
            }
            if (o + 1 < cap) {
                out[o++] = *p;
            }
            p++;
        }
        while (*p && !is_delim(*p)) {
            p++;
        }
    } else {
        while (*p && (last || !is_delim(*p))) {
            if (o + 1 < cap) {
                out[o++] = *p;
            }
            p++;
        }
        while (o > 0 && (out[o - 1] == ' ' || out[o - 1] == '\t')) {
            o--;
        }
    }
    out[o] = '\0';
    if (*p && is_delim(*p)) {
        return p + 1;
    }
    return NULL;
}

static bool number_like(const char *s)
{
    char n[DIR_NUMBER_MAX];
    dir_core_normalize(s, n, sizeof(n));
    if (n[0] == '\0') {
        return false;
    }
    for (const char *p = n; *p; p++) {
        if (!(isdigit((unsigned char)*p) || *p == '+' || *p == '*' || *p == '#')) {
            return false;
        }
    }
    return true;
}

int dir_csv_parse_line(const char *line, char *number, size_t ncap, char *name, size_t mcap)
{
    char buf[DIR_LINE_MAX];
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
        len--;
    }
    if (len >= sizeof(buf)) {
        return -1;
    }
    memcpy(buf, line, len);
    buf[len] = '\0';
    const char *p = buf;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (*p == '\0' || *p == '#') {
        return 1;
    }
    if (starts_with_ci(p, "number,") || starts_with_ci(p, "name,")) {
        return 1;   /* header line of an export from a spreadsheet */
    }
    char f1[DIR_LINE_MAX], f2[DIR_LINE_MAX];
    const char *rest = csv_field(p, f1, sizeof(f1), false);
    if (rest == NULL) {
        return -1;
    }
    csv_field(rest, f2, sizeof(f2), true);
    const char *num = f1, *nm = f2;
    if (!number_like(f1) && number_like(f2)) {
        num = f2;
        nm = f1;
    }
    char n[DIR_NUMBER_MAX], c[DIR_NAME_MAX];
    dir_core_normalize(num, n, sizeof(n));
    dir_core_clean_name(nm, c, sizeof(c));
    if (n[0] == '\0' || c[0] == '\0') {
        return -1;
    }
    snprintf(number, ncap, "%s", n);
    snprintf(name, mcap, "%s", c);
    return 0;
}

size_t dir_csv_format_line(const dir_contact_t *c, char *buf, size_t cap)
{
    bool quote = strpbrk(c->name, ",;\"\t") != NULL;
    size_t o = 0;
#define PUT(ch) do { if (o + 1 < cap) { buf[o] = (ch); } o++; } while (0)
    for (const char *p = c->number; *p; p++) {
        PUT(*p);
    }
    PUT(',');
    if (quote) {
        PUT('"');
    }
    for (const char *p = c->name; *p; p++) {
        if (*p == '"') {
            PUT('"');
        }
        PUT(*p);
    }
    if (quote) {
        PUT('"');
    }
#undef PUT
    if (cap > 0) {
        buf[o < cap ? o : cap - 1] = '\0';
    }
    return o;
}

/* Calls fn(line) for each line of text (NUL-free, not necessarily terminated). */
static void for_each_line(const char *text, size_t len, void (*fn)(const char *, void *), void *ctx)
{
    char line[DIR_LINE_MAX + 2];
    size_t i = 0;
    while (i < len) {
        size_t j = i;
        while (j < len && text[j] != '\n') {
            j++;
        }
        size_t l = j - i;
        if (l > DIR_LINE_MAX) {
            l = DIR_LINE_MAX + 1;   /* too long: parse_line rejects it */
        }
        memcpy(line, text + i, l);
        line[l] = '\0';
        fn(line, ctx);
        i = j + 1;
    }
}

typedef struct {
    dir_ctab_t *t;
    size_t added, bad;
    bool complete;
} ctab_parse_ctx_t;

static void ctab_line(const char *line, void *vctx)
{
    ctab_parse_ctx_t *x = vctx;
    char num[DIR_NUMBER_MAX], name[DIR_NAME_MAX];
    if (strncmp(line, "#end", 4) == 0) {
        x->complete = true;
        return;
    }
    int r = dir_csv_parse_line(line, num, sizeof(num), name, sizeof(name));
    if (r == 0) {
        if (dir_ctab_set(x->t, num, name) >= 0) {
            x->added++;
        } else {
            x->bad++;
        }
    } else if (r < 0) {
        x->bad++;
    }
}

size_t dir_ctab_parse(dir_ctab_t *t, const char *text, size_t len, size_t *bad, bool *complete)
{
    ctab_parse_ctx_t x = { .t = t };
    for_each_line(text, len, ctab_line, &x);
    if (bad) {
        *bad = x.bad;
    }
    if (complete) {
        *complete = x.complete;
    }
    return x.added;
}

int dir_ctab_write(const dir_ctab_t *t, FILE *f)
{
    char line[DIR_LINE_MAX];
    if (fputs("# sipwire contacts: number,name\n", f) < 0) {
        return -1;
    }
    for (size_t i = 0; i < t->n; i++) {
        dir_csv_format_line(&t->v[i], line, sizeof(line));
        if (fputs(line, f) < 0 || fputc('\n', f) < 0) {
            return -1;
        }
    }
    return fputs("#end\n", f) < 0 ? -1 : 0;
}

/* ---- speed dial ---- */

bool dir_speed_set_tab(dir_speed_tab_t *t, int digit, const char *number)
{
    if (digit < DIR_SPEED_FIRST || digit > DIR_SPEED_LAST) {
        return false;
    }
    dir_core_normalize(number, t->num[digit - DIR_SPEED_FIRST], DIR_NUMBER_MAX);
    return true;
}

typedef struct {
    dir_speed_tab_t *t;
    size_t n;
    bool complete;
} speed_ctx_t;

static void speed_line(const char *line, void *vctx)
{
    speed_ctx_t *x = vctx;
    if (strncmp(line, "#end", 4) == 0) {
        x->complete = true;
    } else if (line[0] >= '0' && line[0] <= '9' && line[1] == ',' &&
               dir_speed_set_tab(x->t, line[0] - '0', line + 2)) {
        x->n++;
    }
}

size_t dir_speed_parse(dir_speed_tab_t *t, const char *text, size_t len, bool *complete)
{
    speed_ctx_t x = { .t = t };
    for_each_line(text, len, speed_line, &x);
    if (complete) {
        *complete = x.complete;
    }
    return x.n;
}

int dir_speed_write(const dir_speed_tab_t *t, FILE *f)
{
    if (fputs("# sipwire speed dial: digit,number\n", f) < 0) {
        return -1;
    }
    for (int d = DIR_SPEED_FIRST; d <= DIR_SPEED_LAST; d++) {
        const char *n = t->num[d - DIR_SPEED_FIRST];
        if (n[0] && fprintf(f, "%d,%s\n", d, n) < 0) {
            return -1;
        }
    }
    return fputs("#end\n", f) < 0 ? -1 : 0;
}

/* ---- history ---- */

void dir_hist_add(dir_hist_t *h, const dir_call_t *c)
{
    if (h->cap == 0) {
        return;
    }
    dir_call_t *e = &h->e[h->head];
    *e = *c;
    e->number[sizeof(e->number) - 1] = '\0';
    e->name[sizeof(e->name) - 1] = '\0';
    h->head = (h->head + 1) % h->cap;
    if (h->count < h->cap) {
        h->count++;
    }
}

const dir_call_t *dir_hist_get(const dir_hist_t *h, size_t i)
{
    if (i >= h->count) {
        return NULL;
    }
    return &h->e[(h->head + h->cap - 1 - i) % h->cap];
}

unsigned dir_hist_unseen_missed(const dir_hist_t *h)
{
    unsigned n = 0;
    for (size_t i = 0; i < h->count; i++) {
        const dir_call_t *c = dir_hist_get(h, i);
        if (c->direction == DIR_CALL_IN && c->result == DIR_RES_MISSED && !c->seen) {
            n++;
        }
    }
    return n;
}

const dir_call_t *dir_hist_last_unseen_missed(const dir_hist_t *h)
{
    for (size_t i = 0; i < h->count; i++) {
        const dir_call_t *c = dir_hist_get(h, i);
        if (c->direction == DIR_CALL_IN && c->result == DIR_RES_MISSED && !c->seen) {
            return c;
        }
    }
    return NULL;
}

const dir_call_t *dir_hist_last_dialed(const dir_hist_t *h)
{
    for (size_t i = 0; i < h->count; i++) {
        const dir_call_t *c = dir_hist_get(h, i);
        if (c->direction == DIR_CALL_OUT && c->number[0]) {
            return c;
        }
    }
    return NULL;
}

bool dir_hist_mark_seen(dir_hist_t *h)
{
    bool changed = false;
    for (size_t i = 0; i < h->count; i++) {
        dir_call_t *c = &h->e[(h->head + h->cap - 1 - i) % h->cap];
        if (!c->seen) {
            c->seen = true;
            changed = true;
        }
    }
    return changed;
}

static uint32_t crc32_sw(const uint8_t *p, size_t n)
{
    uint32_t crc = 0xFFFFFFFFu;
    while (n--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

static void put_le(uint8_t *p, uint64_t v, int bytes)
{
    for (int i = 0; i < bytes; i++) {
        p[i] = (uint8_t)(v >> (8 * i));
    }
}

static uint64_t get_le(const uint8_t *p, int bytes)
{
    uint64_t v = 0;
    for (int i = 0; i < bytes; i++) {
        v |= (uint64_t)p[i] << (8 * i);
    }
    return v;
}

size_t dir_hist_size(const dir_hist_t *h)
{
    return DIR_HIST_HDR_SIZE + h->count * DIR_HIST_REC_SIZE;
}

size_t dir_hist_serialize(const dir_hist_t *h, uint8_t *buf, size_t cap)
{
    size_t need = dir_hist_size(h);
    if (cap < need) {
        return 0;
    }
    uint8_t *rec = buf + DIR_HIST_HDR_SIZE;
    for (size_t k = 0; k < h->count; k++) {     /* oldest first */
        const dir_call_t *c = dir_hist_get(h, h->count - 1 - k);
        memset(rec, 0, DIR_HIST_REC_SIZE);
        rec[0] = c->direction;
        rec[1] = c->result;
        rec[2] = c->seen;
        put_le(rec + 4, (uint64_t)(int64_t)c->start, 8);
        put_le(rec + 12, c->duration_s, 4);
        memcpy(rec + 16, c->number, DIR_NUMBER_MAX);
        memcpy(rec + 48, c->name, DIR_NAME_MAX);
        rec += DIR_HIST_REC_SIZE;
    }
    memcpy(buf, "SWH1", 4);
    put_le(buf + 4, h->count, 2);
    put_le(buf + 6, 0, 2);
    put_le(buf + 8, crc32_sw(buf + DIR_HIST_HDR_SIZE, h->count * DIR_HIST_REC_SIZE), 4);
    return need;
}

int dir_hist_deserialize(dir_hist_t *h, const uint8_t *buf, size_t len)
{
    h->head = 0;
    h->count = 0;
    if (len < DIR_HIST_HDR_SIZE || memcmp(buf, "SWH1", 4) != 0) {
        return -1;
    }
    size_t count = (size_t)get_le(buf + 4, 2);
    if (len != DIR_HIST_HDR_SIZE + count * DIR_HIST_REC_SIZE ||
        crc32_sw(buf + DIR_HIST_HDR_SIZE, count * DIR_HIST_REC_SIZE) != (uint32_t)get_le(buf + 8, 4)) {
        return -1;
    }
    const uint8_t *rec = buf + DIR_HIST_HDR_SIZE;
    for (size_t k = 0; k < count; k++, rec += DIR_HIST_REC_SIZE) {
        if (rec[0] > DIR_CALL_OUT || rec[1] > DIR_RES_DND) {
            h->head = 0;
            h->count = 0;
            return -1;
        }
        dir_call_t c = {
            .direction = rec[0],
            .result = rec[1],
            .seen = rec[2] != 0,
            .start = (time_t)(int64_t)get_le(rec + 4, 8),
            .duration_s = (uint32_t)get_le(rec + 12, 4),
        };
        memcpy(c.number, rec + 16, DIR_NUMBER_MAX);
        memcpy(c.name, rec + 48, DIR_NAME_MAX);
        dir_hist_add(h, &c);    /* keeps the newest cap entries if the file holds more */
    }
    return 0;
}
