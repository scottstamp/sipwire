#pragma once

/* Pure logic of the two locks (no LVGL, no IDF): host-tested in test_host/ui_lock.
 *
 * Unlock gesture: a knob at the left end of a track has to be dragged to the right end.
 * Chosen over a long press because a cheek resting on the screen during a call easily holds
 * one spot for seconds, but does not slide a small knob ~170 px sideways in a straight line.
 * A resistive panel (XPT2046) reports the average of all contact points, so a second contact
 * (cheek + ear) makes the reported point jump: a step longer than UI_SWIPE_MAX_STEP or a
 * drift off the track by more than UI_SWIPE_MAX_DY cancels the gesture until the next press. */

#include <stdbool.h>
#include <stdint.h>

#include "ui_model.h"

#define UI_SWIPE_MAX_STEP  60   /* px between two samples (~33 ms apart); more = jump, abort */
#define UI_SWIPE_MAX_DY    40   /* px the finger may leave the press row */
#define UI_SWIPE_END_SLACK 6    /* px short of the end that still counts as the end */

typedef enum {
    UI_SWIPE_IDLE,      /* no press on the knob */
    UI_SWIPE_DRAGGING,  /* knob follows the finger (pos) */
    UI_SWIPE_ABORTED,   /* jump / off the track: ignored until the next press */
    UI_SWIPE_DONE,      /* reached the end: unlock */
} ui_swipe_state_t;

typedef struct {
    ui_swipe_state_t state;
    int32_t travel;         /* knob travel in px (track width - knob width - margins) */
    int32_t x0, y0;         /* press point */
    int32_t last_x, last_y;
    int32_t pos;            /* knob offset 0..travel */
} ui_swipe_t;

/** A press landed on the knob at (x, y), screen coordinates. */
void ui_swipe_press(ui_swipe_t *s, int32_t travel, int32_t x, int32_t y);

/** A new sample while pressed. Returns the new state; `pos` follows while DRAGGING and is
 *  0 after an abort. DONE is returned once, when the end is reached. */
ui_swipe_state_t ui_swipe_move(ui_swipe_t *s, int32_t x, int32_t y);

/** Finger lifted (or the press was lost): back to IDLE, knob home. */
void ui_swipe_release(ui_swipe_t *s);

/** In-call touch lock: may the call screen lock now? An answered, not held, foreground
 *  call on the handset (not the loudspeaker), no waiting second call (its prompt must be
 *  answerable), and a lock time configured (model call_touch_lock_s > 0). */
bool ui_call_lock_eligible(const ui_model_t *m);
