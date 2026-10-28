#pragma once

#include "lvgl.h"
#include "ui_actions.h"
#include "ui_model.h"

/* All functions: caller holds the LVGL lock. */

/** Build screens. `actions` must outlive the UI. */
void ui_init(lv_display_t *disp, const ui_actions_t *actions);

/** Push a new model snapshot; UI diffs against the previous and switches
 *  screens (e.g. incoming call) as needed. */
void ui_update(const ui_model_t *model);

typedef enum {
    UI_KEY_PRESS,
    UI_KEY_LONG,
    UI_KEY_REPEAT,
} ui_key_action_t;

/** Physical keypad input ('0'-'9', '*', '#'). */
void ui_key(char key, ui_key_action_t action);
