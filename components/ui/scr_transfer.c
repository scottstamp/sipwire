/* Transfer target picker (opened from the in-call screen).
 *
 * One screen, four modes:
 *   LIST      the BLF extensions (label, ext, live lamp) in a scrolling list and an
 *             "Other number..." button pinned below it
 *   ENTRY     numeric entry for a number that is not a BLF key
 *   CHOICE    "Transfer now" (blind) / "Talk first" (attended) for the picked target
 *   PROGRESS  after a pick: what the phone reports (trying / ringing). Blind: the
 *             result arrives as the ended screen (success) or the call screen with the
 *             failure shown (the phone keeps / resumes the original call). Attended:
 *             the router moves to the call screen as soon as the consultation call
 *             exists (xfer CONSULT).
 * The model is the single source of truth for the lamps and the progress. */

#include "ui_priv.h"

#include <stdio.h>
#include <string.h>

#define NUM_MAX    32
#define ROW_H      44
#define LIST_Y     72
#define LIST_H     196
#define NO_ANSWER_MS 6000

typedef enum {
    MODE_LIST,
    MODE_ENTRY,
    MODE_CHOICE,
    MODE_PROGRESS,
} mode_t_;

typedef struct {
    lv_obj_t *lamp;
    lv_obj_t *btn;
    char ext[UI_BLF_EXT_LEN];
} row_t;

typedef struct {
    mode_t_ mode;
    bool from_entry;           /* CHOICE was reached from ENTRY (back goes there) */

    lv_obj_t *header;
    lv_obj_t *title;
    lv_obj_t *back;

    /* LIST */
    lv_obj_t *list_box;        /* list + other button */
    lv_obj_t *list;
    lv_obj_t *no_keys;
    row_t rows[UI_BLF_MAX];
    int n_rows;
    char list_sig[UI_BLF_MAX * 8];

    /* ENTRY */
    lv_obj_t *entry_box;
    lv_obj_t *num;
    char buf[NUM_MAX + 1];
    size_t len;

    /* CHOICE */
    lv_obj_t *choice_box;
    lv_obj_t *c_name;
    lv_obj_t *c_sub;
    lv_obj_t *c_state;
    char target[UI_BLF_EXT_LEN > NUM_MAX + 1 ? UI_BLF_EXT_LEN : NUM_MAX + 1];

    /* PROGRESS */
    lv_obj_t *prog_box;
    lv_obj_t *p_title;
    lv_obj_t *p_name;
    lv_obj_t *p_status;
    bool attended;
    ui_xfer_state_t sent_state;
    uint32_t sent_at;
} xfer_t;

static xfer_t *s_x;

static void on_delete(lv_event_t *e)
{
    xfer_t *x = lv_event_get_user_data(e);
    if (s_x == x) {
        s_x = NULL;
    }
    lv_free(x);
}

/* ---- shared helpers (declared in ui_priv.h) -------------------------------- */

const char *ui_target_name(const ui_model_t *m, const char *ext)
{
    for (int i = 0; i < m->blf_count && i < UI_BLF_MAX; i++) {
        if (strcmp(m->blf[i].ext, ext) == 0 && m->blf[i].label[0]) {
            return m->blf[i].label;
        }
    }
    return ext;
}

const char *ui_xfer_text(const ui_model_t *m, char *buf, size_t cap, bool *bad)
{
    const char *who = ui_target_name(m, m->xfer_target);
    *bad = false;
    switch (m->xfer) {
    case UI_XFER_TRYING:
        snprintf(buf, cap, "Transferring to %s...", who);
        return buf;
    case UI_XFER_ACCEPTED:
        snprintf(buf, cap, "Transfer accepted...");
        return buf;
    case UI_XFER_PROGRESS:
        snprintf(buf, cap, "Transfer: %s is ringing", who);
        return buf;
    case UI_XFER_DONE:
        snprintf(buf, cap, "Transferred to %s", who);
        return buf;
    case UI_XFER_FAILED:
        *bad = true;
        if (m->xfer_code > 0) {
            snprintf(buf, cap, "Transfer failed: %d", m->xfer_code);
        } else {
            snprintf(buf, cap, "Transfer failed");
        }
        return buf;
    case UI_XFER_IDLE:
    case UI_XFER_CONSULT:
    default:
        return NULL;
    }
}

static lv_color_t lamp_color(ui_blf_state_t st)
{
    switch (st) {
    case UI_BLF_IDLE:    return UI_COL_OK;
    case UI_BLF_RINGING: return UI_COL_WARN;
    case UI_BLF_BUSY:    return UI_COL_RED;
    case UI_BLF_UNKNOWN:
    default:             return lv_color_hex(0x7D8996);
    }
}

static const char *state_text(ui_blf_state_t st)
{
    switch (st) {
    case UI_BLF_IDLE:    return "Available";
    case UI_BLF_RINGING: return "Ringing";
    case UI_BLF_BUSY:    return "Busy";
    case UI_BLF_UNKNOWN:
    default:             return "";
    }
}

static const ui_blf_t *find_blf(const ui_model_t *m, const char *ext)
{
    for (int i = 0; i < m->blf_count && i < UI_BLF_MAX; i++) {
        if (strcmp(m->blf[i].ext, ext) == 0) {
            return &m->blf[i];
        }
    }
    return NULL;
}

/* ---- modes ------------------------------------------------------------------ */

static void choice_refresh(xfer_t *x, const ui_model_t *m)
{
    const ui_blf_t *b = find_blf(m, x->target);
    const char *name = ui_target_name(m, x->target);
    ui_fit_set(x->c_name, name, &lv_font_montserrat_32, strcmp(name, x->target) == 0);
    ui_fit_set(x->c_sub, strcmp(name, x->target) == 0 ? "" : x->target, &lv_font_montserrat_14,
               true);
    if (b && b->state != UI_BLF_UNKNOWN) {
        ui_fit_set(x->c_state, state_text(b->state), &lv_font_montserrat_14, false);
        ui_fit_set_color(x->c_state, b->state == UI_BLF_IDLE ? UI_COL_OK : lamp_color(b->state));
    } else {
        ui_fit_set(x->c_state, "", &lv_font_montserrat_14, false);
    }
}

static void set_mode(xfer_t *x, mode_t_ mode)
{
    x->mode = mode;
    lv_obj_set_hidden(x->header, mode == MODE_ENTRY);
    lv_obj_set_hidden(x->list_box, mode != MODE_LIST);
    lv_obj_set_hidden(x->entry_box, mode != MODE_ENTRY);
    lv_obj_set_hidden(x->choice_box, mode != MODE_CHOICE);
    lv_obj_set_hidden(x->prog_box, mode != MODE_PROGRESS);
    lv_label_set_text(x->title, mode == MODE_PROGRESS ? "Transfer" : "Transfer to");
    if (mode == MODE_CHOICE) {
        choice_refresh(x, ui_model());
    }
}

static void pick(xfer_t *x, const char *target, bool from_entry)
{
    snprintf(x->target, sizeof(x->target), "%s", target);
    x->from_entry = from_entry;
    set_mode(x, MODE_CHOICE);
}

static void entry_refresh(xfer_t *x)
{
    ui_fit_set(x->num, x->buf, &lv_font_montserrat_32, true);
}

static void entry_append(xfer_t *x, char c)
{
    if (x->len < NUM_MAX) {
        x->buf[x->len++] = c;
        x->buf[x->len] = '\0';
        entry_refresh(x);
    }
}

static void entry_open(xfer_t *x, char first)
{
    x->len = 0;
    x->buf[0] = '\0';
    if (first) {
        entry_append(x, first);
    } else {
        entry_refresh(x);
    }
    set_mode(x, MODE_ENTRY);
}

/* ---- events ------------------------------------------------------------------ */

static void on_back(lv_event_t *e)
{
    xfer_t *x = lv_event_get_user_data(e);
    switch (x->mode) {
    case MODE_CHOICE:
        set_mode(x, x->from_entry ? MODE_ENTRY : MODE_LIST);
        break;
    case MODE_ENTRY:
        set_mode(x, MODE_LIST);
        break;
    case MODE_LIST:
    case MODE_PROGRESS:
    default:
        ui_goto(UI_SCR_CALL, NULL);
        break;
    }
}

static void on_row(lv_event_t *e)
{
    xfer_t *x = s_x;
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (x && i >= 0 && i < x->n_rows) {
        pick(x, x->rows[i].ext, false);
    }
}

static void on_other(lv_event_t *e)
{
    entry_open(lv_event_get_user_data(e), 0);
}

static void on_entry_key(char key, bool long_press, void *user)
{
    entry_append(user, long_press ? '+' : key);
}

static void on_bksp(lv_event_t *e)
{
    xfer_t *x = lv_event_get_user_data(e);
    if (lv_event_get_code(e) == LV_EVENT_LONG_PRESSED) {
        x->len = 0;
        x->buf[0] = '\0';
    } else if (x->len) {
        x->buf[--x->len] = '\0';
    }
    entry_refresh(x);
}

static void on_entry_ok(lv_event_t *e)
{
    xfer_t *x = lv_event_get_user_data(e);
    if (x->len) {
        pick(x, x->buf, true);
    }
}

static void send(xfer_t *x, bool attended)
{
    const ui_model_t *m = ui_model();
    x->attended = attended;
    x->sent_state = m->xfer;
    x->sent_at = lv_tick_get();
    const char *name = ui_target_name(m, x->target);
    ui_fit_set(x->p_name, name, &lv_font_montserrat_32, strcmp(name, x->target) == 0);
    lv_label_set_text(x->p_title, attended ? "Calling" : "Transferring to");
    ui_fit_set(x->p_status, attended ? "Placing consultation call..." : "Trying...",
               &lv_font_montserrat_14, false);
    ui_fit_set_color(x->p_status, UI_COL_WARN);
    set_mode(x, MODE_PROGRESS);
    if (attended) {
        ui_act_transfer_attended_start(x->target);
    } else {
        ui_act_transfer_blind(x->target);
    }
}

static void on_blind(lv_event_t *e)
{
    send(lv_event_get_user_data(e), false);
}

static void on_attended(lv_event_t *e)
{
    send(lv_event_get_user_data(e), true);
}

static void on_to_call(lv_event_t *e)
{
    (void)e;
    ui_goto(UI_SCR_CALL, NULL);
}

/* ---- construction -------------------------------------------------------------- */

static void list_build(xfer_t *x, const ui_model_t *m)
{
    lv_obj_clean(x->list);
    x->n_rows = 0;
    char sig[sizeof(x->list_sig)] = "";
    for (int i = 0; i < m->blf_count && i < UI_BLF_MAX; i++) {
        const ui_blf_t *b = &m->blf[i];
        row_t *r = &x->rows[x->n_rows];
        snprintf(r->ext, sizeof(r->ext), "%s", b->ext);

        lv_obj_t *btn = ui_btn_create(x->list, UI_BTN_NEUTRAL, NULL, 0, 0, 228, ROW_H);
        lv_obj_set_style_flex_grow(btn, 0, 0);
        lv_obj_set_style_text_align(btn, LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_add_event_cb(btn, on_row, LV_EVENT_CLICKED, (void *)(intptr_t)x->n_rows);
        r->btn = btn;

        r->lamp = ui_box_create(btn, 10, (ROW_H - 14) / 2, 14, 14);
        lv_obj_set_style_radius(r->lamp, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(r->lamp, LV_OPA_COVER, 0);

        const char *name = b->label[0] ? b->label : b->ext;
        lv_obj_t *nl = ui_fit_create(btn, 32, 6, b->label[0] ? 128 : 190, 32, UI_COL_TEXT);
        ui_fit_set(nl, name, &lv_font_montserrat_24, false);
        lv_obj_set_style_text_align(lv_obj_get_child(nl, 0), LV_TEXT_ALIGN_LEFT, 0);
        if (b->label[0]) {
            lv_obj_t *el = ui_fit_create(btn, 164, 13, 58, 18, UI_COL_DIM);
            ui_fit_set(el, b->ext, &lv_font_montserrat_14, true);
            lv_obj_set_style_text_align(lv_obj_get_child(el, 0), LV_TEXT_ALIGN_RIGHT, 0);
        }
        x->n_rows++;
        size_t sl = strlen(sig);
        snprintf(sig + sl, sizeof(sig) - sl, "%s,", b->ext);
    }
    snprintf(x->list_sig, sizeof(x->list_sig), "%s", sig);
    lv_obj_set_hidden(x->no_keys, x->n_rows > 0);
}

static lv_obj_t *transfer_create(const char *arg)
{
    (void)arg;
    xfer_t *x = lv_zalloc(sizeof(*x));
    if (!x) {
        return NULL;
    }
    lv_obj_t *scr = ui_screen_create();
    lv_obj_add_event_cb(scr, on_delete, LV_EVENT_DELETE, x);
    ui_statusbar_create(scr);

    x->back = ui_header_create(scr, "Transfer to", UI_BAR_H, &x->title);
    x->header = lv_obj_get_parent(x->back);
    lv_obj_add_event_cb(x->back, on_back, LV_EVENT_CLICKED, x);

    /* LIST */
    x->list_box = ui_box_create(scr, 0, UI_BAR_H + UI_HEADER_H, UI_W, UI_H - UI_BAR_H - UI_HEADER_H);
    x->list = lv_obj_create(x->list_box);
    lv_obj_remove_style_all(x->list);
    lv_obj_set_pos(x->list, 6, 4);
    lv_obj_set_size(x->list, 228, LIST_H - 4);
    lv_obj_set_flex_flow(x->list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(x->list, 4, 0);
    lv_obj_set_scroll_dir(x->list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(x->list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scrollable(x->list, true);
    x->no_keys = ui_label_create(x->list_box, &lv_font_montserrat_14, UI_COL_DIM,
                                 "No BLF keys set.\nUse \"Other number...\".");
    lv_obj_set_width(x->no_keys, UI_W);
    lv_obj_set_style_text_align(x->no_keys, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(x->no_keys, 0, 60);
    lv_obj_t *other = ui_btn_create(x->list_box, UI_BTN_BLUE, "Other number...", 6,
                                    LIST_H + 6, 228, 44);
    lv_obj_add_event_cb(other, on_other, LV_EVENT_CLICKED, x);

    /* ENTRY: own back button, the header is hidden */
    x->entry_box = ui_box_create(scr, 0, UI_BAR_H, UI_W, UI_H - UI_BAR_H);
    lv_obj_t *eb = ui_btn_create(x->entry_box, UI_BTN_NEUTRAL, LV_SYMBOL_LEFT, 6, 4, 44, 40);
    lv_obj_add_event_cb(eb, on_back, LV_EVENT_CLICKED, x);
    x->num = ui_fit_create(x->entry_box, 56, 6, 122, 36, UI_COL_TEXT);
    lv_obj_t *bk = ui_btn_create(x->entry_box, UI_BTN_NEUTRAL, LV_SYMBOL_BACKSPACE, 184, 4, 50, 40);
    lv_obj_add_event_cb(bk, on_bksp, LV_EVENT_CLICKED, x);
    lv_obj_add_event_cb(bk, on_bksp, LV_EVENT_LONG_PRESSED, x);
    lv_obj_t *pad = ui_keypad_create(x->entry_box, 6, 48, 228, 172, on_entry_key, x);
    lv_obj_set_style_pad_row(pad, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_column(pad, 6, LV_PART_MAIN);
    lv_obj_t *ok = ui_btn_create(x->entry_box, UI_BTN_GREEN, "Next", 6, 226, 228, 48);
    lv_obj_add_event_cb(ok, on_entry_ok, LV_EVENT_CLICKED, x);

    /* CHOICE */
    x->choice_box = ui_box_create(scr, 0, UI_BAR_H + UI_HEADER_H, UI_W, UI_H - UI_BAR_H - UI_HEADER_H);
    x->c_name = ui_fit_create(x->choice_box, 6, 8, 228, 40, UI_COL_TEXT);
    x->c_sub = ui_fit_create(x->choice_box, 6, 50, 228, 18, UI_COL_DIM);
    x->c_state = ui_fit_create(x->choice_box, 6, 70, 228, 18, UI_COL_DIM);
    lv_obj_t *now = ui_icon_btn_create(x->choice_box, UI_BTN_BLUE, "Transfer now",
                                       &lv_font_montserrat_24, 0, "Hand over right away",
                                       &lv_font_montserrat_14, 6, 98, 228, 64);
    lv_obj_add_event_cb(now, on_blind, LV_EVENT_CLICKED, x);
    lv_obj_t *talk = ui_icon_btn_create(x->choice_box, UI_BTN_GREEN, "Talk first",
                                        &lv_font_montserrat_24, 0, "Consult, then complete",
                                        &lv_font_montserrat_14, 6, 170, 228, 64);
    lv_obj_add_event_cb(talk, on_attended, LV_EVENT_CLICKED, x);

    /* PROGRESS */
    x->prog_box = ui_box_create(scr, 0, UI_BAR_H + UI_HEADER_H, UI_W, UI_H - UI_BAR_H - UI_HEADER_H);
    x->p_title = ui_label_create(x->prog_box, &lv_font_montserrat_24, UI_COL_DIM, "");
    lv_obj_set_width(x->p_title, UI_W);
    lv_obj_set_style_text_align(x->p_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(x->p_title, 0, 30);
    x->p_name = ui_fit_create(x->prog_box, 6, 66, 228, 40, UI_COL_TEXT);
    x->p_status = ui_fit_create(x->prog_box, 6, 118, 228, 40, UI_COL_WARN);
    lv_obj_t *tc = ui_btn_create(x->prog_box, UI_BTN_NEUTRAL, "Back to call", 6, 170, 228, 48);
    lv_obj_add_event_cb(tc, on_to_call, LV_EVENT_CLICKED, NULL);

    s_x = x;
    list_build(x, ui_model());
    set_mode(x, MODE_LIST);
    return scr;
}

/* ---- update ------------------------------------------------------------------- */

static void transfer_update(const ui_model_t *m)
{
    xfer_t *x = s_x;
    if (!x) {
        return;
    }

    /* BLF list: rebuild when the extensions change, else just repaint the lamps */
    char sig[sizeof(x->list_sig)] = "";
    for (int i = 0; i < m->blf_count && i < UI_BLF_MAX; i++) {
        size_t sl = strlen(sig);
        snprintf(sig + sl, sizeof(sig) - sl, "%s,", m->blf[i].ext);
    }
    if (strcmp(sig, x->list_sig) != 0) {
        list_build(x, m);
    }
    for (int i = 0; i < x->n_rows && i < m->blf_count; i++) {
        lv_obj_set_style_bg_color(x->rows[i].lamp, lamp_color(m->blf[i].state), 0);
    }
    if (x->mode == MODE_CHOICE) {
        choice_refresh(x, m);
    }

    if (x->mode == MODE_PROGRESS) {
        bool changed = m->xfer != x->sent_state;
        char buf[72];
        bool bad;
        const char *t = ui_xfer_text(m, buf, sizeof(buf), &bad);
        if (changed && t) {
            ui_fit_set(x->p_status, t, &lv_font_montserrat_24, false);
            ui_fit_set_color(x->p_status, bad ? UI_COL_BAD : UI_COL_WARN);
        }
        if (changed && m->xfer == UI_XFER_FAILED) {
            ui_goto(UI_SCR_CALL, NULL); /* the call screen shows the failure */
            return;
        }
        if (!changed && lv_tick_elaps(x->sent_at) > NO_ANSWER_MS) {
            ui_toast("No response");
            set_mode(x, MODE_CHOICE);
        }
    }
}

static void transfer_key(char key, ui_key_action_t action)
{
    xfer_t *x = s_x;
    if (!x || action != UI_KEY_PRESS || !ui_is_dial_key(key)) {
        return;
    }
    if (x->mode == MODE_LIST) {
        entry_open(x, key);
    } else if (x->mode == MODE_ENTRY) {
        entry_append(x, key);
    }
}

const ui_screen_ops_t ui_ops_transfer = {transfer_create, transfer_update, transfer_key, NULL,
                                         NULL};
