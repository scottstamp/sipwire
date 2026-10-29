/* Virtual list: only the rows in view (plus two) exist as LVGL objects. The list is a
 * scrollable box whose content height is made by an invisible spacer at the end; on every
 * scroll step the pool rows are moved to the items now in view and re-bound.
 * 200 contacts cost ~8 row widgets instead of 200. */

#include "ui_priv.h"

typedef struct {
    const ui_vlist_ops_t *ops;
    void *user;
    int32_t row_h;
    int32_t count;
    int32_t first;          /* item index of pool row 0 */
    int32_t n_pool;
    lv_obj_t *spacer;
    lv_obj_t *rows[];
} vlist_t;

static vlist_t *vl_of(lv_obj_t *list)
{
    return lv_obj_get_user_data(list);
}

static void place(lv_obj_t *list, vlist_t *v)
{
    int32_t sy = lv_obj_get_scroll_y(list);
    int32_t first = sy / v->row_h;
    int32_t max_first = v->count - v->n_pool + 1;
    if (first > max_first) {
        first = max_first;
    }
    if (first < 0) {
        first = 0;
    }
    v->first = first;
    for (int32_t i = 0; i < v->n_pool; i++) {
        lv_obj_t *r = v->rows[i];
        int32_t idx = first + i;
        if (idx >= v->count) {
            lv_obj_set_user_data(r, NULL);
            lv_obj_set_hidden(r, true);
            continue;
        }
        lv_obj_set_hidden(r, false);
        lv_obj_set_y(r, idx * v->row_h + 2);
        lv_obj_set_user_data(r, (void *)(intptr_t)(idx + 1));
        v->ops->bind_row(r, idx, v->user);
    }
}

static void on_scroll(lv_event_t *e)
{
    lv_obj_t *list = lv_event_get_target_obj(e);
    vlist_t *v = vl_of(list);
    if (v && lv_obj_get_scroll_y(list) / v->row_h != v->first) {
        place(list, v);
    }
}

static void on_row_click(lv_event_t *e)
{
    lv_obj_t *row = lv_event_get_target_obj(e);
    lv_obj_t *list = lv_obj_get_parent(row);
    vlist_t *v = vl_of(list);
    int32_t idx = ui_vlist_row_index(row);
    if (v && idx >= 0 && v->ops->clicked) {
        v->ops->clicked(idx, v->user);
    }
}

static void on_delete(lv_event_t *e)
{
    lv_free(vl_of(lv_event_get_target_obj(e)));
}

int32_t ui_vlist_row_index(lv_obj_t *row)
{
    return (int32_t)(intptr_t)lv_obj_get_user_data(row) - 1;
}

lv_obj_t *ui_vlist_create(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h, int32_t row_h,
                          const ui_vlist_ops_t *ops, void *user)
{
    int32_t n_pool = h / row_h + 2;
    vlist_t *v = lv_zalloc(sizeof(*v) + (size_t)n_pool * sizeof(lv_obj_t *));
    if (!v) {
        return NULL;
    }
    v->ops = ops;
    v->user = user;
    v->row_h = row_h;
    v->n_pool = n_pool;

    lv_obj_t *list = ui_box_create(parent, x, y, w, h);
    lv_obj_set_scrollable(list, true);
    lv_obj_set_clickable(list, true);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_user_data(list, v);
    lv_obj_add_event_cb(list, on_scroll, LV_EVENT_SCROLL, NULL);
    lv_obj_add_event_cb(list, on_delete, LV_EVENT_DELETE, NULL);

    v->spacer = ui_box_create(list, 0, 0, 1, 1);
    for (int32_t i = 0; i < n_pool; i++) {
        lv_obj_t *r = ui_btn_create(list, UI_BTN_NEUTRAL, NULL, 0, 0, w, row_h - 4);
        lv_obj_add_event_cb(r, on_row_click, LV_EVENT_CLICKED, NULL);
        lv_obj_set_hidden(r, true);
        ops->make_row(r, user);
        v->rows[i] = r;
    }
    return list;
}

void ui_vlist_set_count(lv_obj_t *list, int32_t count)
{
    vlist_t *v = vl_of(list);
    if (!v) {
        return;
    }
    v->count = count;
    lv_obj_set_y(v->spacer, count > 0 ? count * v->row_h - 1 : 0);
    lv_obj_update_layout(list);
    lv_obj_scroll_to_y(list, lv_obj_get_scroll_y(list), LV_ANIM_OFF);   /* clamp after a shrink */
    place(list, v);
}

void ui_vlist_refresh(lv_obj_t *list)
{
    vlist_t *v = vl_of(list);
    if (v) {
        place(list, v);
    }
}

void ui_vlist_set_height(lv_obj_t *list, int32_t h)
{
    vlist_t *v = vl_of(list);
    lv_obj_set_height(list, h);
    if (v) {
        lv_obj_update_layout(list);
        place(list, v);
    }
}
