#include "ui_lock_logic.h"

#include <stdlib.h>

void ui_swipe_press(ui_swipe_t *s, int32_t travel, int32_t x, int32_t y)
{
    s->state = UI_SWIPE_DRAGGING;
    s->travel = travel > 0 ? travel : 1;
    s->x0 = s->last_x = x;
    s->y0 = s->last_y = y;
    s->pos = 0;
}

ui_swipe_state_t ui_swipe_move(ui_swipe_t *s, int32_t x, int32_t y)
{
    if (s->state != UI_SWIPE_DRAGGING) {
        return s->state;
    }
    if (abs(x - s->last_x) > UI_SWIPE_MAX_STEP || abs(y - s->last_y) > UI_SWIPE_MAX_STEP ||
        abs(y - s->y0) > UI_SWIPE_MAX_DY) {
        s->state = UI_SWIPE_ABORTED;
        s->pos = 0;
        return s->state;
    }
    s->last_x = x;
    s->last_y = y;
    int32_t pos = x - s->x0;
    s->pos = pos < 0 ? 0 : pos > s->travel ? s->travel : pos;
    if (s->pos >= s->travel - UI_SWIPE_END_SLACK) {
        s->pos = s->travel;
        s->state = UI_SWIPE_DONE;
    }
    return s->state;
}

void ui_swipe_release(ui_swipe_t *s)
{
    s->state = UI_SWIPE_IDLE;
    s->pos = 0;
}

bool ui_call_lock_eligible(const ui_model_t *m)
{
    return m->call_touch_lock_s > 0 && m->call == UI_CALL_ACTIVE && !m->speaker &&
           m->second_call != UI_CALL_RINGING_IN;
}
