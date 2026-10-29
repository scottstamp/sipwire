#pragma once

/* Pure text formatting for the history screen (no LVGL; host-tested in test_host/ui_fmt). */

#include <stdbool.h>
#include <stddef.h>

struct tm;

/** "14:03" / "2:03 PM" for today, "Mon" within the last week, else "Oct 12". `t` is the
 *  entry's time and `now` the current time, both broken down in local time. */
void ui_fmt_when(char *buf, size_t cap, const struct tm *t, const struct tm *now, bool clock_24h);
