#pragma once

/* Pure helpers for the history screen (no LVGL; host-tested in test_host/ui_fmt). */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ui_model.h"

struct tm;

/** "14:03" / "2:03 PM" for today, "Mon" within the last week, else "Oct 12". `t` is the
 *  entry's time and `now` the current time, both broken down in local time. */
void ui_fmt_when(char *buf, size_t cap, const struct tm *t, const struct tm *now, bool clock_24h);

/** After a refetch of the history (newest first): how many entries were added in front of the
 *  old newest entry `old_first`, i.e. its index in `items[0..n)`; -1 if it is gone (cleared). */
int32_t ui_hist_shift(const ui_call_t *old_first, const ui_call_t *items, int32_t n);

/** Carry the "shown as new" flags over a refetch: `fresh[0..old_n)` belonged to the old list,
 *  `shift` (ui_hist_shift) entries were added in front. New entries get !seen, the others keep
 *  their old flag (so missed calls stay highlighted while the screen is open even though
 *  opening it marked them seen). Entries beyond the old list get !seen. */
void ui_hist_carry_fresh(bool *fresh, int32_t old_n, int32_t shift, const ui_call_t *items, int32_t n);
