#include "keypad_scan.h"

#include <stdbool.h>
#include <string.h>

#define ALL_KEYS_MASK ((uint16_t)((1u << KEYPAD_NUM_KEYS) - 1u))

/* True once now has reached deadline. Wrap-safe for uint32 milliseconds. */
static bool reached(uint32_t now, uint32_t deadline)
{
    return (int32_t)(now - deadline) >= 0;
}

static void push(keypad_scan_event_t *out, size_t max, size_t *n,
                 keypad_scan_type_t type, uint8_t key)
{
    if (*n < max) {
        out[*n].type = type;
        out[*n].key = key;
    }
    (*n)++;
}

void keypad_scan_reset(keypad_scan_state_t *st)
{
    memset(st, 0, sizeof(*st));
}

size_t keypad_scan_step(keypad_scan_state_t *st, uint16_t raw_mask, uint32_t now_ms,
                        keypad_scan_event_t *out, size_t max)
{
    size_t n = 0;
    raw_mask &= ALL_KEYS_MASK;
    const uint16_t changed = (uint16_t)(raw_mask ^ st->raw_prev);

    for (uint8_t k = 0; k < KEYPAD_NUM_KEYS; k++) {
        const uint16_t bit = (uint16_t)(1u << k);
        const bool raw = (raw_mask & bit) != 0;
        bool down = (st->stable & bit) != 0;

        if (changed & bit) {
            st->since_ms[k] = now_ms;
        }

        /* Debounce: the raw level must hold for DEBOUNCE_MS before it counts. */
        if (raw != down && reached(now_ms, st->since_ms[k] + KEYPAD_DEBOUNCE_MS)) {
            if (raw) {
                st->stable |= bit;
                st->long_sent &= (uint16_t)~bit;
                st->press_ms[k] = now_ms;
                push(out, max, &n, KEYPAD_SCAN_PRESS, k);
            } else {
                st->stable &= (uint16_t)~bit;
                st->long_sent &= (uint16_t)~bit;
                push(out, max, &n, KEYPAD_SCAN_RELEASE, k);
            }
            down = raw;
        }

        if (!down) {
            continue;
        }

        if (!(st->long_sent & bit)) {
            if (reached(now_ms, st->press_ms[k] + KEYPAD_LONG_MS)) {
                st->long_sent |= bit;
                st->next_rep_ms[k] = now_ms + KEYPAD_REPEAT_MS;
                push(out, max, &n, KEYPAD_SCAN_LONG, k);
            }
        } else if (reached(now_ms, st->next_rep_ms[k])) {
            /* Advance by the period, not from now, so repeats don't drift.
             * After a long stall, restart the cadence instead of bursting. */
            st->next_rep_ms[k] += KEYPAD_REPEAT_MS;
            if (reached(now_ms, st->next_rep_ms[k])) {
                st->next_rep_ms[k] = now_ms + KEYPAD_REPEAT_MS;
            }
            push(out, max, &n, KEYPAD_SCAN_REPEAT, k);
        }
    }

    st->raw_prev = raw_mask;
    return n;
}
