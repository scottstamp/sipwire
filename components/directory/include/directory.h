#pragma once

/* Contacts, speed dial and call history, persisted on the `storage` FAT partition
 * (wear-levelled, mounted at /data). All functions are thread-safe (one mutex) and
 * may be called from the LVGL, console and phone tasks.
 *
 * Numbers are normalised everywhere (stored, searched, looked up): `sip:`/`sips:`/`tel:`
 * prefixes, `<...>` wrappers, `@host` and `;params` are dropped, and for numeric strings
 * spaces, dashes, dots, slashes and parentheses are removed (keeps digits, `*`, `#`, a
 * leading `+`). A user part with letters is kept as is, lower-cased ("sip:Alice@x" ->
 * "alice"). dir_normalize() exposes this.
 *
 * Data is kept in RAM (PSRAM when present) and saved after every change: a
 * contacts/speed-dial change rewrites a small CSV file, a history append rewrites ~8 KB.
 * Writes go to a temp file that is then renamed over the real one. If the partition
 * cannot be mounted the directory still works from RAM and the mutators return the error. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include "esp_err.h"

#define DIR_NAME_MAX      32    /* bytes incl. NUL (names are truncated on a UTF-8 boundary) */
#define DIR_NUMBER_MAX    32    /* bytes incl. NUL (normalised) */
#define DIR_CONTACTS_MAX  200
#define DIR_HISTORY_MAX   100
#define DIR_SPEED_FIRST   2     /* speed dial keys 2..9; 1 is voicemail (setting vm_number) */
#define DIR_SPEED_LAST    9

typedef struct {
    char name[DIR_NAME_MAX];
    char number[DIR_NUMBER_MAX];
} dir_contact_t;

typedef enum {
    DIR_CALL_IN = 0,
    DIR_CALL_OUT = 1,
} dir_direction_t;

typedef enum {
    DIR_RES_ANSWERED = 0,   /* connected (outgoing: the far end answered) */
    DIR_RES_MISSED = 1,     /* incoming: not answered; outgoing: no answer / failed */
    DIR_RES_REJECTED = 2,   /* incoming: declined by the user */
    DIR_RES_DND = 3,        /* incoming: auto-rejected by do-not-disturb */
} dir_result_t;

typedef struct {
    uint8_t direction;      /* dir_direction_t */
    uint8_t result;         /* dir_result_t */
    bool seen;              /* missed calls: false until dir_history_mark_seen() */
    time_t start;           /* 0 = unknown (no NTP yet) */
    uint32_t duration_s;    /* talk time, 0 unless answered */
    char number[DIR_NUMBER_MAX];
    char name[DIR_NAME_MAX];    /* caller ID name or contact name; may be empty */
} dir_call_t;

/** Mount /data (formats the partition on the first, failed mount), load all files, register
 *  the console commands (contact, speed, history). Call after settings_init(). Returns ESP_OK
 *  even if the mount fails (RAM-only mode, error logged). */
esp_err_t dir_init(void);

/** Normalise a number/URI into `out` (see above). Empty result for junk. */
void dir_normalize(const char *in, char *out, size_t cap);

/* ---- contacts (sorted by name, case-insensitive, then number; one entry per number) ---- */

/** Add, or update the name of the contact with that (normalised) number.
 *  ESP_ERR_INVALID_ARG (empty number/name), ESP_ERR_NO_MEM when DIR_CONTACTS_MAX is reached. */
esp_err_t dir_contact_set(const char *number, const char *name);
/** ESP_ERR_NOT_FOUND if there is no such number. */
esp_err_t dir_contact_del(const char *number);
size_t dir_contact_count(void);
/** Copy up to `max` contacts, sorted, starting at index `start`. Returns the number copied. */
size_t dir_contact_list(dir_contact_t *out, size_t start, size_t max);
/** Contacts whose name contains `query` (case-insensitive) or whose number contains the
 *  normalised query. Matches at the start of the name/number come first, then the rest, each
 *  group sorted. Empty query = everything. Returns the number copied (<= max). */
size_t dir_contact_search(const char *query, dir_contact_t *out, size_t max);
/** Caller-ID lookup for an incoming number or SIP URI. False if unknown. */
bool dir_contact_lookup(const char *number, char *name, size_t name_cap);
/** Parse CSV text (`number,name` per line; `name,number`, `;`/tab delimiters, quotes and `#`
 *  comments tolerated) and add/update the contacts, saving once. Bad lines are counted. */
esp_err_t dir_contact_import_csv(const char *text, size_t len, size_t *added, size_t *bad);
/** Format one contact as a CSV line without newline (`number,"name"`; quotes only if needed). */
size_t dir_contact_csv_line(const dir_contact_t *c, char *buf, size_t cap);

/* ---- speed dial (digit DIR_SPEED_FIRST..DIR_SPEED_LAST) ---- */

/** Set the number (normalised); NULL or "" clears. ESP_ERR_INVALID_ARG for a bad digit. */
esp_err_t dir_speed_set(int digit, const char *number);
/** ESP_ERR_NOT_FOUND if the slot is empty. */
esp_err_t dir_speed_get(int digit, char *number, size_t cap);

/* ---- call history (ring of DIR_HISTORY_MAX, oldest dropped) ---- */

/** Append. `c->name` empty -> filled from the contacts; `seen` is set for everything except
 *  missed incoming calls. */
esp_err_t dir_history_add(const dir_call_t *c);
size_t dir_history_count(void);
/** Newest first, starting at index `start` (0 = newest). Returns the number copied. */
size_t dir_history_list(dir_call_t *out, size_t start, size_t max);
esp_err_t dir_history_clear(void);
/** Missed incoming calls not yet seen (for a badge). */
unsigned dir_history_unseen_missed(void);
/** Mark all missed calls seen (call when the history screen is opened). */
esp_err_t dir_history_mark_seen(void);
