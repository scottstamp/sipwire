#pragma once

/* Pure touch-calibration math and sample capture for scr_calibrate.c. No LVGL,
 * no IDF: host-tested in test_host/ui_touch_cal.
 *
 * Four targets are touched in the order TL, TR, BR, BL, each `inset` pixels
 * from the screen edges. Raw values are in the space described at
 * ui_touch_cal_t (after swap_xy, before mirror/calibration). */

#include <stdbool.h>
#include <stdint.h>

#include "ui_actions.h"

#define UI_TCAL_POINTS 4
#define UI_TCAL_MIN_SPAN 1000    /* raw counts between left/right and top/bottom */
#define UI_TCAL_MAX_SKEW 400     /* raw counts two points of one edge may differ */
#define UI_TCAL_MIN_SAMPLES 3    /* shorter presses are ignored */
#define UI_TCAL_RAW_LO (-1000)   /* extrapolated edge values must stay in this range */
#define UI_TCAL_RAW_HI 5100

typedef struct {
    int32_t x, y;
} ui_tcal_raw_t;

typedef enum {
    UI_TCAL_OK = 0,
    UI_TCAL_ERR_ARG,   /* bad screen size / inset */
    UI_TCAL_ERR_SPAN,  /* left/right or top/bottom too close together */
    UI_TCAL_ERR_SKEW,  /* points of one edge disagree (swapped axes, bad taps) */
    UI_TCAL_ERR_RANGE, /* extrapolation leaves the ADC range */
} ui_tcal_status_t;

/** Screen position of target `i` (0 TL, 1 TR, 2 BR, 3 BL) for a w x h screen. */
void ui_tcal_target(int i, int w, int h, int inset, int32_t *x, int32_t *y);

/** Calibration from the raw averages measured at the four targets: x from the
 *  left (TL, BL) vs right (TR, BR) points, y from top (TL, TR) vs bottom
 *  (BL, BR), extrapolated linearly to screen x = 0 / w-1 and y = 0 / h-1.
 *  `out` is only written on UI_TCAL_OK. */
ui_tcal_status_t ui_tcal_compute(const ui_tcal_raw_t raw[UI_TCAL_POINTS], int w, int h,
                                 int inset, ui_touch_cal_t *out);

/** Raw -> screen coordinate on one axis of `size` pixels, clamped to
 *  0..size-1, using raw values `at_first` (pixel 0) and `at_last` (pixel
 *  size-1). Same arithmetic as the display driver. */
int32_t ui_tcal_map(int32_t raw, int16_t at_first, int16_t at_last, int32_t size);

/** Averages the samples of one touch; the point is accepted on release. */
typedef struct {
    int64_t sx, sy;
    uint32_t n;
} ui_tcal_cap_t;

void ui_tcal_cap_reset(ui_tcal_cap_t *c);

/** Feed one sample. Returns true when a release closed a press of at least
 *  UI_TCAL_MIN_SAMPLES samples; `out` then holds the average. A short press
 *  is dropped silently. One tap yields at most one point. */
bool ui_tcal_cap_feed(ui_tcal_cap_t *c, int32_t ax, int32_t ay, bool pressed,
                      ui_tcal_raw_t *out);
