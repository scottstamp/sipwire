#include "ui_touch_cal.h"

static int32_t iabs(int32_t v)
{
    return v < 0 ? -v : v;
}

static int32_t round_d(double v)
{
    return (int32_t)(v >= 0 ? v + 0.5 : v - 0.5);
}

void ui_tcal_target(int i, int w, int h, int inset, int32_t *x, int32_t *y)
{
    static const uint8_t RIGHT[UI_TCAL_POINTS] = {0, 1, 1, 0};
    static const uint8_t BOTTOM[UI_TCAL_POINTS] = {0, 0, 1, 1};
    i = (i < 0 || i >= UI_TCAL_POINTS) ? 0 : i;
    *x = RIGHT[i] ? w - 1 - inset : inset;
    *y = BOTTOM[i] ? h - 1 - inset : inset;
}

/* Line through (s_lo, a_lo) and (s_hi, a_hi), evaluated at 0 and size-1. */
static bool axis(double a_lo, double a_hi, int32_t s_lo, int32_t s_hi, int size, int16_t *at_first,
                 int16_t *at_last)
{
    double slope = (a_hi - a_lo) / (double)(s_hi - s_lo);
    int32_t v0 = round_d(a_lo - slope * s_lo);
    int32_t v1 = round_d(a_lo + slope * (double)((size - 1) - s_lo));
    if (v0 < UI_TCAL_RAW_LO || v0 > UI_TCAL_RAW_HI || v1 < UI_TCAL_RAW_LO || v1 > UI_TCAL_RAW_HI) {
        return false;
    }
    *at_first = (int16_t)v0;
    *at_last = (int16_t)v1;
    return true;
}

ui_tcal_status_t ui_tcal_compute(const ui_tcal_raw_t raw[UI_TCAL_POINTS], int w, int h, int inset,
                                 ui_touch_cal_t *out)
{
    if (!raw || !out || inset < 0 || w - 1 - 2 * inset < 10 || h - 1 - 2 * inset < 10) {
        return UI_TCAL_ERR_ARG;
    }
    const ui_tcal_raw_t *tl = &raw[0], *tr = &raw[1], *br = &raw[2], *bl = &raw[3];

    double xl = (tl->x + bl->x) / 2.0, xr = (tr->x + br->x) / 2.0;
    double yt = (tl->y + tr->y) / 2.0, yb = (bl->y + br->y) / 2.0;
    if (iabs(round_d(xr - xl)) < UI_TCAL_MIN_SPAN || iabs(round_d(yb - yt)) < UI_TCAL_MIN_SPAN) {
        return UI_TCAL_ERR_SPAN;
    }
    if (iabs(tl->x - bl->x) > UI_TCAL_MAX_SKEW || iabs(tr->x - br->x) > UI_TCAL_MAX_SKEW ||
        iabs(tl->y - tr->y) > UI_TCAL_MAX_SKEW || iabs(bl->y - br->y) > UI_TCAL_MAX_SKEW) {
        return UI_TCAL_ERR_SKEW;
    }

    int32_t x_lo, x_hi, y_lo, y_hi;
    ui_tcal_target(0, w, h, inset, &x_lo, &y_lo);
    ui_tcal_target(2, w, h, inset, &x_hi, &y_hi);

    ui_touch_cal_t c;
    if (!axis(xl, xr, x_lo, x_hi, w, &c.x_min, &c.x_max) ||
        !axis(yt, yb, y_lo, y_hi, h, &c.y_min, &c.y_max)) {
        return UI_TCAL_ERR_RANGE;
    }
    *out = c;
    return UI_TCAL_OK;
}

int32_t ui_tcal_map(int32_t raw, int16_t at_first, int16_t at_last, int32_t size)
{
    if (size < 2 || at_first == at_last) {
        return 0;
    }
    int64_t v = ((int64_t)raw - at_first) * (size - 1) / ((int64_t)at_last - at_first);
    return v < 0 ? 0 : (v > size - 1 ? size - 1 : (int32_t)v);
}

void ui_tcal_cap_reset(ui_tcal_cap_t *c)
{
    c->sx = 0;
    c->sy = 0;
    c->n = 0;
}

bool ui_tcal_cap_feed(ui_tcal_cap_t *c, int32_t ax, int32_t ay, bool pressed, ui_tcal_raw_t *out)
{
    if (pressed) {
        c->sx += ax;
        c->sy += ay;
        c->n++;
        return false;
    }
    bool ok = c->n >= UI_TCAL_MIN_SAMPLES;
    if (ok && out) {
        out->x = (int32_t)((c->sx + (int64_t)c->n / 2) / (int64_t)c->n);
        out->y = (int32_t)((c->sy + (int64_t)c->n / 2) / (int64_t)c->n);
    }
    ui_tcal_cap_reset(c);
    return ok;
}
