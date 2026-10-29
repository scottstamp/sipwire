/* Pure text formatting for the history screen (host-tested in test_host/ui_fmt). */

#include "ui_fmt.h"

#include <stdint.h>
#include <stdio.h>
#include <time.h>

/* Days since 1970-01-01 of a civil date (proleptic Gregorian). */
static int32_t day_number(int year, int mon /* 1-12 */, int day)
{
    int y = mon <= 2 ? year - 1 : year;
    int era = (y >= 0 ? y : y - 399) / 400;
    int yoe = y - era * 400;
    int doy = (153 * (mon + (mon > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

void ui_fmt_when(char *buf, size_t cap, const struct tm *t, const struct tm *now, bool clock_24h)
{
    static const char *const WDAY[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char *const MON[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                      "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    int32_t diff = day_number(now->tm_year + 1900, now->tm_mon + 1, now->tm_mday) -
                   day_number(t->tm_year + 1900, t->tm_mon + 1, t->tm_mday);
    if (diff == 0) {
        if (clock_24h) {
            snprintf(buf, cap, "%02d:%02d", t->tm_hour, t->tm_min);
        } else {
            int h = t->tm_hour % 12 == 0 ? 12 : t->tm_hour % 12;
            snprintf(buf, cap, "%d:%02d %s", h, t->tm_min, t->tm_hour < 12 ? "AM" : "PM");
        }
    } else if (diff > 0 && diff < 7 && t->tm_wday >= 0 && t->tm_wday < 7) {
        snprintf(buf, cap, "%s", WDAY[t->tm_wday]);
    } else if (t->tm_mon >= 0 && t->tm_mon < 12) {
        snprintf(buf, cap, "%s %d", MON[t->tm_mon], t->tm_mday);
    } else {
        snprintf(buf, cap, "-");
    }
}
