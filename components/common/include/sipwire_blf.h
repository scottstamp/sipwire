#pragma once

/* BLF key list, the `blf` setting: comma separated entries "ext[:label]",
 * e.g. "1002:Bob,1003:Front desk,1004". Shared by storage (validation),
 * phone (subscriptions) and the console. Pure C (host-tested in
 * test_host/phone). */

#include <stdbool.h>
#include <stddef.h>

#define SIPWIRE_BLF_MAX        8     /* entries */
#define SIPWIRE_BLF_EXT_MAX    32    /* incl. NUL */
#define SIPWIRE_BLF_LABEL_MAX  24    /* incl. NUL */
#define SIPWIRE_BLF_LIST_MAX   448   /* whole list incl. NUL (settings field): 8 full entries fit */

typedef struct {
    char ext[SIPWIRE_BLF_EXT_MAX];      /* dialable: 0-9 A-Z a-z * # + . _ - */
    char label[SIPWIRE_BLF_LABEL_MAX];  /* may be empty; printable, no ',' or ':' */
} sipwire_blf_entry_t;

/** Parse a list. White space around items, extensions and labels is
 *  trimmed; empty items (",,", trailing ",") are skipped. Returns the number
 *  of entries (0 for an empty list), or -1 if an entry is invalid (bad
 *  extension character, empty or too long extension, label too long or with
 *  a control character), an extension repeats, or there are more than `max`.
 *  `out` may be NULL (validation only). */
int sipwire_blf_parse(const char *list, sipwire_blf_entry_t *out, int max);

/** "ext[:label],..." for n entries. Returns false if it does not fit. */
bool sipwire_blf_format(const sipwire_blf_entry_t *e, int n, char *out, size_t cap);

/** Extension / label character rules, for input forms. */
bool sipwire_blf_ext_char_ok(char c);
bool sipwire_blf_label_char_ok(char c);
