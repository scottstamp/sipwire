#pragma once

/* Pure logic of the directory component: no IDF dependencies except the types and
 * esp_err_t from directory.h (host tests stub esp_err.h). Not thread-safe; directory.c
 * wraps it in a mutex. Tested in test_host/directory. */

#include <stdio.h>
#include "directory.h"

#define DIR_LINE_MAX 160

void dir_core_normalize(const char *in, char *out, size_t cap);
/* Strip control characters and surrounding blanks, truncate to cap-1 bytes on a UTF-8 boundary. */
void dir_core_clean_name(const char *in, char *out, size_t cap);

/* ---- contacts table (sorted array, caller-provided storage) ---- */
typedef struct {
    dir_contact_t *v;
    size_t n;
    size_t cap;
} dir_ctab_t;

enum { DIR_C_UPDATED = 0, DIR_C_ADDED = 1, DIR_C_INVALID = -1, DIR_C_FULL = -2 };
int dir_ctab_set(dir_ctab_t *t, const char *number, const char *name);
bool dir_ctab_del(dir_ctab_t *t, const char *number);
int dir_ctab_find(const dir_ctab_t *t, const char *number);     /* normalises; index or -1 */
/* Indices into t->v, prefix matches first. */
size_t dir_ctab_search(const dir_ctab_t *t, const char *query, size_t *idx, size_t max);

/* One CSV line. 0 = ok, 1 = blank/comment (skipped), -1 = bad. */
int dir_csv_parse_line(const char *line, char *number, size_t ncap, char *name, size_t mcap);
size_t dir_csv_format_line(const dir_contact_t *c, char *buf, size_t cap);
/* Whole text; returns lines added/updated. *complete = the "#end" trailer was seen. */
size_t dir_ctab_parse(dir_ctab_t *t, const char *text, size_t len, size_t *bad, bool *complete);
int dir_ctab_write(const dir_ctab_t *t, FILE *f);   /* 0 ok; ends with "#end" */

/* ---- speed dial ---- */
typedef struct {
    char num[DIR_SPEED_LAST - DIR_SPEED_FIRST + 1][DIR_NUMBER_MAX];
} dir_speed_tab_t;
bool dir_speed_set_tab(dir_speed_tab_t *t, int digit, const char *number);
size_t dir_speed_parse(dir_speed_tab_t *t, const char *text, size_t len, bool *complete);
int dir_speed_write(const dir_speed_tab_t *t, FILE *f);

/* ---- history ring (storage provided by caller, cap entries) ---- */
typedef struct {
    dir_call_t *e;
    size_t cap;
    size_t head;    /* next write position */
    size_t count;
} dir_hist_t;

void dir_hist_add(dir_hist_t *h, const dir_call_t *c);
const dir_call_t *dir_hist_get(const dir_hist_t *h, size_t i);  /* 0 = newest; NULL if out of range */
unsigned dir_hist_unseen_missed(const dir_hist_t *h);
bool dir_hist_mark_seen(dir_hist_t *h);                          /* true if anything changed */

#define DIR_HIST_REC_SIZE 80
#define DIR_HIST_HDR_SIZE 12
size_t dir_hist_size(const dir_hist_t *h);                      /* bytes dir_hist_serialize needs */
size_t dir_hist_serialize(const dir_hist_t *h, uint8_t *buf, size_t cap);   /* oldest first; 0 if cap too small */
int dir_hist_deserialize(dir_hist_t *h, const uint8_t *buf, size_t len);    /* 0 ok, -1 corrupt (h left empty) */
