#pragma once

/* Pure debounce and event logic for the 3x4 keypad matrix.
 * No IDF headers: compiled into the firmware and into host tests (test_host/keypad).
 *
 * Keys are indexed row-major, index = row * KEYPAD_COLS + col. Bit i of a raw
 * mask is 1 while key i reads pressed. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define KEYPAD_ROWS          4
#define KEYPAD_COLS          3
#define KEYPAD_NUM_KEYS      (KEYPAD_ROWS * KEYPAD_COLS)
#define KEYPAD_DEBOUNCE_MS   30
#define KEYPAD_LONG_MS       800   /* measured from the debounced PRESS */
#define KEYPAD_REPEAT_MS     150   /* after LONG, while held */

/* A key yields at most one event per step, so this bounds one step's output. */
#define KEYPAD_MAX_EVENTS    KEYPAD_NUM_KEYS

typedef enum {
    KEYPAD_SCAN_PRESS,
    KEYPAD_SCAN_RELEASE,
    KEYPAD_SCAN_LONG,
    KEYPAD_SCAN_REPEAT,
} keypad_scan_type_t;

typedef struct {
    keypad_scan_type_t type;
    uint8_t key;                           /* 0..KEYPAD_NUM_KEYS-1 */
} keypad_scan_event_t;

typedef struct {
    uint16_t raw_prev;                     /* last raw sample */
    uint16_t stable;                       /* debounced: 1 = pressed */
    uint16_t long_sent;                    /* LONG already emitted for this press */
    uint32_t since_ms[KEYPAD_NUM_KEYS];    /* when the raw bit last changed */
    uint32_t press_ms[KEYPAD_NUM_KEYS];    /* when the debounced PRESS was emitted */
    uint32_t next_rep_ms[KEYPAD_NUM_KEYS]; /* next REPEAT deadline */
} keypad_scan_state_t;

/* Clear all state: nothing pressed. */
void keypad_scan_reset(keypad_scan_state_t *st);

/* True when nothing is pressed or settling: the last raw sample was all
 * released and no key is debounced down. Steps would emit nothing until a key
 * reads pressed again, so the caller may stop sampling and wait for one. */
bool keypad_scan_idle(const keypad_scan_state_t *st);

/* Feed one raw sample taken at now_ms (uint32 ms, wraps). Writes up to max
 * events to out and returns the total number generated. If the return value
 * exceeds max, the excess events were dropped; pass KEYPAD_MAX_EVENTS entries
 * to avoid that. Sample at least every ~24 days or wrap comparisons break. */
size_t keypad_scan_step(keypad_scan_state_t *st, uint16_t raw_mask, uint32_t now_ms,
                        keypad_scan_event_t *out, size_t max);
