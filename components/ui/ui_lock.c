/* Lock widgets shared by the in-call touch lock (scr_call.c) and the keypad lock
 * (scr_home.c): a padlock icon drawn from two boxes (the Montserrat symbol set has none),
 * a touch shield that swallows every press, and the slide-to-unlock control whose gesture
 * rules live in ui_lock_logic.c. */

#include "ui_priv.h"

#include "ui_lock_logic.h"

#define KNOB_PAD 3      /* knob inset in the track */

typedef struct {
    lv_obj_t *knob;
    lv_obj_t *text;
    ui_swipe_t sw;
    ui_unlock_cb_t cb;
    void *user;
} unlock_t;

lv_obj_t *ui_lock_icon_create(lv_obj_t *parent, int32_t h, lv_color_t color)
{
    int32_t u = h / 9 > 2 ? h / 9 : 2;         /* stroke */
    int32_t bw = h * 7 / 9, bh = h * 5 / 9;    /* body */
    lv_obj_t *icon = ui_box_create(parent, 0, 0, bw, h);
    lv_obj_t *shackle = ui_box_create(icon, u, 0, bw - 2 * u, h - bh + u * 2);
    lv_obj_set_style_border_width(shackle, u, 0);
    lv_obj_set_style_border_color(shackle, color, 0);
    lv_obj_set_style_border_opa(shackle, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(shackle, LV_RADIUS_CIRCLE, 0);
    lv_obj_t *body = ui_box_create(icon, 0, h - bh, bw, bh);
    lv_obj_set_style_bg_color(body, color, 0);
    lv_obj_set_style_bg_opa(body, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(body, u, 0);
    return icon;
}

void ui_lock_icon_set_color(lv_obj_t *icon, lv_color_t color)
{
    ui_style_set_color(lv_obj_get_child(icon, 0), LV_STYLE_BORDER_COLOR, color, 0);
    ui_style_set_color(lv_obj_get_child(icon, 1), LV_STYLE_BG_COLOR, color, 0);
}

lv_obj_t *ui_touch_shield_create(lv_obj_t *parent)
{
    lv_obj_t *o = ui_box_create(parent, 0, 0, lv_pct(100), lv_pct(100));
    lv_obj_set_clickable(o, true);      /* every press ends here, nothing under it acts */
    return o;
}

static void knob_place(unlock_t *u)
{
    lv_obj_set_x(u->knob, KNOB_PAD + u->sw.pos);
    int32_t fade = u->sw.travel > 0 ? u->sw.pos * 255 / u->sw.travel : 0;
    lv_obj_set_style_text_opa(u->text, (lv_opa_t)(255 - (fade > 255 ? 255 : fade)), 0);
}

static void on_knob(lv_event_t *e)
{
    unlock_t *u = lv_event_get_user_data(e);
    lv_event_code_t code = lv_event_get_code(e);
    lv_point_t p = {0, 0};
    lv_indev_t *indev = lv_indev_active();
    if (indev) {
        lv_indev_get_point(indev, &p);
    }
    if (code == LV_EVENT_PRESSED) {
        lv_obj_t *track = lv_obj_get_parent(u->knob);
        int32_t travel = lv_obj_get_width(track) - lv_obj_get_width(u->knob) - 2 * KNOB_PAD;
        ui_swipe_press(&u->sw, travel, p.x, p.y);
    } else if (code == LV_EVENT_PRESSING) {
        ui_swipe_state_t before = u->sw.state;
        ui_swipe_state_t st = ui_swipe_move(&u->sw, p.x, p.y);
        if (st != before || st == UI_SWIPE_DRAGGING) {
            knob_place(u);
        }
        if (st == UI_SWIPE_DONE && before != UI_SWIPE_DONE && u->cb) {
            u->cb(u->user);
        }
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        ui_swipe_release(&u->sw);
        knob_place(u);
    }
}

static void on_track_delete(lv_event_t *e)
{
    lv_free(lv_event_get_user_data(e));
}

lv_obj_t *ui_unlock_slider_create(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h,
                                  ui_unlock_cb_t cb, void *user)
{
    unlock_t *u = lv_zalloc(sizeof(*u));
    if (!u) {
        return NULL;
    }
    u->cb = cb;
    u->user = user;

    lv_obj_t *track = ui_box_create(parent, x, y, w, h);
    lv_obj_set_style_bg_color(track, UI_COL_BAR, 0);
    lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(track, UI_COL_SURFACE2, 0);
    lv_obj_set_style_border_width(track, 1, 0);
    lv_obj_set_style_radius(track, h / 2, 0);
    lv_obj_add_event_cb(track, on_track_delete, LV_EVENT_DELETE, u);

    int32_t kh = h - 2 * KNOB_PAD;
    int32_t kw = kh + 16;
    u->text = ui_label_create(track, &lv_font_montserrat_14, UI_COL_DIM, "Slide to unlock " LV_SYMBOL_RIGHT);
    lv_obj_align(u->text, LV_ALIGN_CENTER, (kw + KNOB_PAD) / 2, 0);

    u->knob = ui_box_create(track, KNOB_PAD, KNOB_PAD, kw, kh);
    lv_obj_set_style_bg_color(u->knob, UI_COL_SURFACE2, 0);
    lv_obj_set_style_bg_opa(u->knob, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(u->knob, kh / 2, 0);
    lv_obj_set_clickable(u->knob, true);
    lv_obj_set_user_data(u->knob, u);
    lv_obj_set_press_lock(u->knob, true);   /* keeps the press while the knob lags behind */
    lv_obj_set_ext_click_area(u->knob, 4);
    lv_obj_t *icon = ui_lock_icon_create(u->knob, kh * 11 / 20, UI_COL_WARN);
    lv_obj_center(icon);
    lv_obj_add_event_cb(u->knob, on_knob, LV_EVENT_PRESSED, u);
    lv_obj_add_event_cb(u->knob, on_knob, LV_EVENT_PRESSING, u);
    lv_obj_add_event_cb(u->knob, on_knob, LV_EVENT_RELEASED, u);
    lv_obj_add_event_cb(u->knob, on_knob, LV_EVENT_PRESS_LOST, u);
    return track;
}

void ui_unlock_slider_reset(lv_obj_t *slider)
{
    if (!slider) {
        return;
    }
    lv_obj_t *knob = lv_obj_get_child(slider, 1);
    unlock_t *u = knob ? lv_obj_get_user_data(knob) : NULL;
    if (u) {
        ui_swipe_release(&u->sw);
        knob_place(u);
    }
}
