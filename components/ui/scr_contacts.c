/* Contacts: sorted, searchable list (virtual rows: ui_vlist) + add / edit / delete form
 * with a speed dial key (2-9). Tap a contact = call it; the pencil edits it. The header holds
 * the search field in list mode and the title + Save in edit mode; one on-screen keyboard
 * serves both. The list is a snapshot fetched through ui_actions_t.contacts_search and
 * refetched when the query changes or a contact is saved or deleted. */

#include "ui_priv.h"

#include <stdio.h>
#include <string.h>

#define LIST_Y  (UI_BAR_H + UI_HEADER_H)
#define LIST_H  (UI_H - LIST_Y)
#define ROW_H   48
#define LIST_X  6
#define LIST_W  228
#define KB_H    160
#define FIELD_H 44
#define EDIT_W  44

/* Dial pad keyboard (LVGL's number pad has no '*' or '#'); "abc" switches to letters. */
static const char *const DIAL_KB_MAP[] = {"1", "2", "3", LV_SYMBOL_BACKSPACE, "\n",
                                          "4", "5", "6", LV_SYMBOL_OK, "\n",
                                          "7", "8", "9", "abc", "\n",
                                          "*", "0", "#", LV_SYMBOL_LEFT, LV_SYMBOL_RIGHT, ""};
static const lv_buttonmatrix_ctrl_t DIAL_KB_CTRL[] = {
    1, 1, 1, LV_KEYBOARD_CTRL_BUTTON_FLAGS | 2,
    1, 1, 1, LV_KEYBOARD_CTRL_BUTTON_FLAGS | 2,
    1, 1, 1, LV_KEYBOARD_CTRL_BUTTON_FLAGS | 2,
    1, 1, 1, 1, 1};
static const char NUMBER_CHARS[] = "0123456789*#+._-abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";

/* row children, in creation order */
enum { C_NAME, C_NUM, C_BADGE, C_EDIT };

typedef struct {
    /* header */
    lv_obj_t *back, *search, *add, *title, *save;
    /* list */
    lv_obj_t *list, *empty, *kb;
    ui_contact_t *items;
    int32_t n;
    /* edit form */
    lv_obj_t *form, *name, *number, *speed, *speed_cap, *del;
    bool editing;
    bool is_new;
    bool del_armed;
    char old_number[UI_NUMBER_LEN];
} con_t;

static con_t *s_c;

static void on_delete(lv_event_t *e)
{
    con_t *c = lv_event_get_user_data(e);
    if (s_c == c) {
        s_c = NULL;
    }
    lv_free(c->items);
    lv_free(c);
}

/* ---- list ------------------------------------------------------------------ */

static void refetch(con_t *c)
{
    c->n = (int32_t)ui_act_contacts_search(lv_textarea_get_text(c->search), c->items, UI_CONTACTS_MAX);
    ui_vlist_set_count(c->list, c->n);
    bool searching = lv_textarea_get_text(c->search)[0] != '\0';
    lv_label_set_text(c->empty, searching ? "No matches" : "No contacts\nTap + to add one");
    lv_obj_set_hidden(c->empty, c->n != 0);
}

static void kb_hide(con_t *c)
{
    if (lv_obj_is_hidden(c->kb)) {
        return;
    }
    lv_keyboard_set_textarea(c->kb, NULL);
    lv_obj_set_hidden(c->kb, true);
    ui_vlist_set_height(c->list, LIST_H);
    lv_obj_set_height(c->form, LIST_H);
}

static void kb_show(con_t *c, lv_obj_t *ta, lv_keyboard_mode_t mode)
{
    lv_keyboard_set_mode(c->kb, mode);
    lv_keyboard_set_textarea(c->kb, ta);
    lv_obj_set_hidden(c->kb, false);
    if (c->editing) {
        lv_obj_set_height(c->form, LIST_H - KB_H);
        lv_obj_update_layout(c->form);
        lv_obj_scroll_to_view(lv_obj_get_parent(ta), LV_ANIM_OFF);
    } else {
        ui_vlist_set_height(c->list, LIST_H - KB_H);
    }
}

static void on_edit_btn(lv_event_t *e);

static void make_row(lv_obj_t *row, void *user)
{
    (void)user;
    lv_obj_t *name = ui_fit_create(row, 10, 1, 170, 28, UI_COL_TEXT);
    lv_obj_set_style_text_align(lv_obj_get_child(name, 0), LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_t *num = ui_label_create(row, &lv_font_montserrat_14, UI_COL_DIM, "");
    lv_obj_set_pos(num, 10, 27);
    lv_obj_set_width(num, 128);
    lv_obj_set_height(num, 18);
    lv_label_set_long_mode(num, LV_LABEL_LONG_MODE_DOTS);
    /* speed dial key badge */
    lv_obj_t *badge = ui_box_create(row, 144, 25, 28, 18);
    lv_obj_set_style_bg_color(badge, UI_COL_BLUE, 0);
    lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(badge, 5, 0);
    lv_obj_t *bl = ui_label_create(badge, &lv_font_montserrat_14, UI_COL_TEXT, "");
    lv_obj_center(bl);
    /* edit button: a child button takes its own taps, the row is "call" */
    lv_obj_t *ed = ui_btn_create(row, UI_BTN_NEUTRAL, NULL, LIST_W - EDIT_W - 2, 1, EDIT_W, ROW_H - 6);
    lv_obj_set_style_bg_color(ed, UI_COL_SURFACE2, 0);
    lv_obj_add_event_cb(ed, on_edit_btn, LV_EVENT_CLICKED, NULL);
    lv_obj_t *il = ui_label_create(ed, &lv_font_montserrat_24, UI_COL_TEXT, LV_SYMBOL_EDIT);
    lv_obj_center(il);
}

static void bind_row(lv_obj_t *row, int32_t idx, void *user)
{
    con_t *c = user;
    const ui_contact_t *k = &c->items[idx];
    if (k->name[0]) {
        ui_fit_set_fixed(lv_obj_get_child(row, C_NAME), k->name, &lv_font_montserrat_24);
    } else {
        ui_fit_set(lv_obj_get_child(row, C_NAME), k->number, &lv_font_montserrat_24, true);  /* keeps the end */
    }
    lv_label_set_text(lv_obj_get_child(row, C_NUM), k->name[0] ? k->number : "");
    lv_obj_t *badge = lv_obj_get_child(row, C_BADGE);
    lv_obj_set_hidden(badge, k->speed == 0);
    if (k->speed) {
        char d[2] = {(char)('0' + k->speed), '\0'};
        lv_label_set_text(lv_obj_get_child(badge, 0), d);
    }
}

static void edit_open(con_t *c, const ui_contact_t *k);

static void on_edit_btn(lv_event_t *e)
{
    con_t *c = s_c;
    lv_obj_t *row = lv_obj_get_parent(lv_event_get_target_obj(e));
    int32_t idx = ui_vlist_row_index(row);
    if (c && idx >= 0 && idx < c->n) {
        ui_contact_t k = c->items[idx];
        edit_open(c, &k);
    }
}

static void clicked(int32_t idx, void *user)
{
    con_t *c = user;
    if (idx >= 0 && idx < c->n) {
        ui_act_dial(c->items[idx].number);
    }
}

static const ui_vlist_ops_t OPS = {make_row, bind_row, clicked};

static void on_search_changed(lv_event_t *e)
{
    refetch(lv_event_get_user_data(e));
}

static void on_search_clicked(lv_event_t *e)
{
    con_t *c = lv_event_get_user_data(e);
    kb_show(c, c->search, LV_KEYBOARD_MODE_TEXT_LOWER);
}

static void on_list_scroll_begin(lv_event_t *e)
{
    kb_hide(lv_event_get_user_data(e));   /* scrolling the results wants the room back */
}

/* ---- header modes -------------------------------------------------------------- */

static void set_mode(con_t *c, bool editing)
{
    c->editing = editing;
    kb_hide(c);
    lv_obj_set_hidden(c->search, editing);
    lv_obj_set_hidden(c->add, editing);
    lv_obj_set_hidden(c->title, !editing);
    lv_obj_set_hidden(c->save, !editing);
    lv_obj_set_hidden(c->list, editing);
    lv_obj_set_hidden(c->form, !editing);
    lv_obj_set_hidden(c->empty, editing || c->n != 0);
}

static void on_back(lv_event_t *e)
{
    con_t *c = lv_event_get_user_data(e);
    if (c->editing) {
        set_mode(c, false);
    } else {
        ui_goto(UI_SCR_HOME, NULL);
    }
}

/* ---- edit form --------------------------------------------------------------- */

static void speed_caption(con_t *c)
{
    uint32_t sel = lv_dropdown_get_selected(c->speed);
    ui_contact_t other;
    if (sel > 0 && ui_act_speed_get((int)sel + 1, &other) && strcmp(other.number, c->old_number) != 0 &&
        strcmp(other.number, lv_textarea_get_text(c->number)) != 0) {
        char t[64];
        snprintf(t, sizeof(t), "Speed dial - replaces %s", other.name[0] ? other.name : other.number);
        lv_label_set_text(c->speed_cap, t);
        lv_obj_set_style_text_color(c->speed_cap, UI_COL_WARN, 0);
    } else {
        lv_label_set_text(c->speed_cap, "Speed dial (hold the key 2-9)");
        lv_obj_set_style_text_color(c->speed_cap, UI_COL_DIM, 0);
    }
}

static void del_disarm(con_t *c)
{
    c->del_armed = false;
    lv_label_set_text(lv_obj_get_child(c->del, 0), LV_SYMBOL_TRASH " Delete contact");
}

static void edit_open(con_t *c, const ui_contact_t *k)
{
    c->is_new = k == NULL;
    snprintf(c->old_number, sizeof(c->old_number), "%s", k ? k->number : "");
    lv_textarea_set_text(c->name, k ? k->name : "");
    lv_textarea_set_text(c->number, k ? k->number : "");
    lv_dropdown_set_selected(c->speed, k && k->speed >= UI_SPEED_FIRST ? (uint32_t)k->speed - 1 : 0);
    lv_label_set_text(c->title, k ? "Edit" : "New");
    lv_obj_set_hidden(c->del, k == NULL);
    del_disarm(c);
    speed_caption(c);
    set_mode(c, true);
    lv_obj_scroll_to_y(c->form, 0, LV_ANIM_OFF);
}

static void on_add(lv_event_t *e)
{
    con_t *c = lv_event_get_user_data(e);
    /* a number typed in the search field becomes the new contact's number when it looks like one */
    edit_open(c, NULL);
    const char *q = lv_textarea_get_text(c->search);
    if (q[0] && strspn(q, "0123456789*#+") == strlen(q)) {
        lv_textarea_set_text(c->number, q);
    }
}

static void on_save(lv_event_t *e)
{
    con_t *c = lv_event_get_user_data(e);
    const char *name = lv_textarea_get_text(c->name);
    const char *number = lv_textarea_get_text(c->number);
    if (!number[0]) {
        ui_toast("Enter a number");
        return;
    }
    if (!name[0]) {
        ui_toast("Enter a name");
        return;
    }
    uint32_t sel = lv_dropdown_get_selected(c->speed);
    const char *err = ui_act_contact_save(c->is_new ? NULL : c->old_number, name, number,
                                          sel ? (uint8_t)(sel + 1) : 0);
    if (err) {
        ui_toast(err);
        return;
    }
    ui_toast("Contact saved");
    set_mode(c, false);
    refetch(c);
}

static void on_delete_btn(lv_event_t *e)
{
    con_t *c = lv_event_get_user_data(e);
    if (!c->del_armed) {
        c->del_armed = true;
        lv_label_set_text(lv_obj_get_child(c->del, 0), "Tap again to delete");
        return;
    }
    ui_act_contact_delete(c->old_number);
    ui_toast("Contact deleted");
    set_mode(c, false);
    refetch(c);
}

static void on_speed_changed(lv_event_t *e)
{
    speed_caption(lv_event_get_user_data(e));
}

static void on_field_clicked(lv_event_t *e)
{
    con_t *c = lv_event_get_user_data(e);
    lv_obj_t *ta = lv_event_get_target_obj(e);
    kb_show(c, ta, ta == c->number ? LV_KEYBOARD_MODE_USER_1 : LV_KEYBOARD_MODE_TEXT_LOWER);
}

static void on_form_clicked(lv_event_t *e)
{
    kb_hide(lv_event_get_user_data(e));   /* tap on the empty form area */
}

static void on_kb_done(lv_event_t *e)
{
    kb_hide(lv_event_get_user_data(e));
}

static lv_obj_t *field_create(con_t *c, lv_obj_t *form, const char *caption, const char *ph, uint32_t max_len)
{
    lv_obj_t *cap = ui_label_create(form, &lv_font_montserrat_14, UI_COL_DIM, caption);
    lv_obj_set_style_pad_top(cap, 6, 0);
    lv_obj_t *row = ui_box_create(form, 0, 0, LIST_W, FIELD_H);
    lv_obj_t *ta = lv_textarea_create(row);
    ui_style_textarea(ta);
    lv_obj_set_size(ta, LIST_W, FIELD_H);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_placeholder_text(ta, ph);
    lv_textarea_set_max_length(ta, max_len);
    lv_obj_set_scroll_on_focus(ta, false);
    lv_obj_set_scrollbar_mode(ta, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(ta, on_field_clicked, LV_EVENT_CLICKED, c);
    return ta;
}

static void style_kb(lv_obj_t *kb)
{
    lv_obj_set_size(kb, UI_W, KB_H);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(kb, UI_COL_BG, 0);
    lv_obj_set_style_bg_opa(kb, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(kb, 0, 0);
    lv_obj_set_style_radius(kb, 0, 0);
    lv_obj_set_style_pad_all(kb, 4, 0);
    lv_obj_set_style_pad_gap(kb, 4, 0);
    lv_obj_set_style_bg_color(kb, UI_COL_SURFACE, LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(kb, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_text_color(kb, UI_COL_TEXT, LV_PART_ITEMS);
    lv_obj_set_style_text_font(kb, &lv_font_montserrat_14, LV_PART_ITEMS);
    lv_obj_set_style_radius(kb, 6, LV_PART_ITEMS);
    lv_obj_set_style_border_width(kb, 0, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(kb, 0, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(kb, UI_COL_BLUE, LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(kb, UI_COL_SURFACE2, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(kb, UI_COL_TEXT, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_USER_1, DIAL_KB_MAP, DIAL_KB_CTRL);
}

/* ---- screen ------------------------------------------------------------------- */

static lv_obj_t *contacts_create(const char *arg)
{
    (void)arg;
    con_t *c = lv_zalloc(sizeof(*c));
    if (!c) {
        return NULL;
    }
    c->items = lv_malloc(sizeof(ui_contact_t) * UI_CONTACTS_MAX);
    if (!c->items) {
        lv_free(c);
        return NULL;
    }
    lv_obj_t *scr = ui_screen_create();
    lv_obj_add_event_cb(scr, on_delete, LV_EVENT_DELETE, c);

    /* body: list and (hidden) edit form */
    c->list = ui_vlist_create(scr, LIST_X, LIST_Y, LIST_W, LIST_H, ROW_H, &OPS, c);
    lv_obj_add_event_cb(c->list, on_list_scroll_begin, LV_EVENT_SCROLL_BEGIN, c);
    c->empty = ui_label_create(scr, &lv_font_montserrat_24, UI_COL_DIM, "");
    lv_obj_set_width(c->empty, 220);
    lv_obj_set_style_text_align(c->empty, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(c->empty, LV_ALIGN_TOP_MID, 0, LIST_Y + 24);   /* stays above the keyboard */

    c->form = ui_box_create(scr, 0, LIST_Y, UI_W, LIST_H);
    lv_obj_set_scrollable(c->form, true);
    lv_obj_set_clickable(c->form, true);
    lv_obj_set_scroll_dir(c->form, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(c->form, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(c->form, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(c->form, 6, 0);
    lv_obj_set_style_pad_row(c->form, 2, 0);
    lv_obj_add_event_cb(c->form, on_form_clicked, LV_EVENT_CLICKED, c);
    c->name = field_create(c, c->form, "Name", "Alice Smith", UI_NAME_LEN - 1);
    c->number = field_create(c, c->form, "Number", "1001", UI_NUMBER_LEN - 1);
    lv_textarea_set_accepted_chars(c->number, NUMBER_CHARS);
    c->speed_cap = ui_label_create(c->form, &lv_font_montserrat_14, UI_COL_DIM, "");
    lv_obj_set_style_pad_top(c->speed_cap, 6, 0);
    lv_obj_set_width(c->speed_cap, LIST_W);
    lv_label_set_long_mode(c->speed_cap, LV_LABEL_LONG_MODE_DOTS);
    c->speed = lv_dropdown_create(c->form);
    lv_dropdown_set_options_static(c->speed, "None\n2\n3\n4\n5\n6\n7\n8\n9");
    lv_obj_set_size(c->speed, LIST_W, FIELD_H);
    ui_style_dropdown(c->speed);
    lv_obj_add_event_cb(c->speed, on_speed_changed, LV_EVENT_VALUE_CHANGED, c);
    c->del = ui_btn_create(c->form, UI_BTN_RED, LV_SYMBOL_TRASH " Delete contact", 0, 0, LIST_W, FIELD_H);
    lv_obj_set_style_margin_top(c->del, 14, 0);
    lv_obj_add_event_cb(c->del, on_delete_btn, LV_EVENT_CLICKED, c);
    lv_obj_set_hidden(c->form, true);

    /* header: back | search | add   or   back | title | save */
    c->back = ui_header_create(scr, "", UI_BAR_H, &c->title);
    lv_obj_add_event_cb(c->back, on_back, LV_EVENT_CLICKED, c);
    lv_obj_t *hdr = lv_obj_get_parent(c->back);
    c->search = lv_textarea_create(hdr);
    ui_style_textarea(c->search);
    lv_obj_set_pos(c->search, 52, 2);
    lv_obj_set_size(c->search, UI_W - 52 - 52, 44);
    lv_textarea_set_one_line(c->search, true);
    lv_textarea_set_placeholder_text(c->search, "Search contacts");
    lv_textarea_set_max_length(c->search, UI_NAME_LEN - 1);
    lv_obj_set_scroll_on_focus(c->search, false);
    lv_obj_set_scrollbar_mode(c->search, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(c->search, on_search_changed, LV_EVENT_VALUE_CHANGED, c);
    lv_obj_add_event_cb(c->search, on_search_clicked, LV_EVENT_CLICKED, c);
    c->add = ui_btn_create(hdr, UI_BTN_GREEN, LV_SYMBOL_PLUS, UI_W - 48, 2, 46, 44);
    lv_obj_add_event_cb(c->add, on_add, LV_EVENT_CLICKED, c);
    c->save = ui_btn_create(hdr, UI_BTN_GREEN, "Save", UI_W - 76, 2, 72, 44);
    lv_obj_add_event_cb(c->save, on_save, LV_EVENT_CLICKED, c);
    lv_obj_set_hidden(c->save, true);
    lv_obj_set_hidden(c->title, true);
    ui_statusbar_create(scr);

    c->kb = lv_keyboard_create(scr);
    style_kb(c->kb);
    lv_obj_set_hidden(c->kb, true);
    lv_obj_add_event_cb(c->kb, on_kb_done, LV_EVENT_READY, c);
    lv_obj_add_event_cb(c->kb, on_kb_done, LV_EVENT_CANCEL, c);

    s_c = c;
    refetch(c);
    return scr;
}

static void contacts_update(const ui_model_t *m)
{
    (void)m;
}

static void contacts_key(char key, ui_key_action_t action)
{
    con_t *c = s_c;
    if (!c || action != UI_KEY_PRESS) {
        return;
    }
    if (key == '*') {
        if (!lv_obj_is_hidden(c->kb)) {
            kb_hide(c);
        } else if (c->editing) {
            set_mode(c, false);
        } else {
            ui_goto(UI_SCR_HOME, NULL);
        }
    } else if (!c->editing && lv_obj_is_hidden(c->kb) && ui_is_dial_key(key)) {
        char first[2] = {key, '\0'};
        ui_goto(UI_SCR_DIALER, first);
    }
}

const ui_screen_ops_t ui_ops_contacts = {contacts_create, contacts_update, contacts_key, NULL, NULL};
