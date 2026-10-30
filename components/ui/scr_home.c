/* Idle screen: clock, account, registration state and buttons; with BLF keys configured
 * a panel of up to 8 tiles (tap = dial that extension), and when voicemail is waiting a
 * button that dials the voicemail number. The History button carries a red badge with the
 * number of unseen missed calls; where the layout has the line under the state (no Wi-Fi
 * problem to report there) it also says who called ("Missed (3): Bob"; tap = History).
 *
 * Layout (240x320, status bar 0-21):
 *   no BLF keys    big clock, name, id, state; "N new voicemails" banner when
 *                  waiting; Contacts | History (46 px) above Dial | Settings (62 px)
 *   BLF keys       tiles 2 per row, 44 px high, bottom-aligned above a 48 px
 *                  icon-only button row: Dial, Contacts, History, [Voicemail],
 *                  Settings (4 or 5 buttons). The header shrinks with the number of
 *                  rows: 1 row -> clock, name, id, state; 2 -> clock, name, state;
 *                  3 -> clock, state; 4 -> one line: smaller clock left, short state
 *                  right. */

#include "ui_priv.h"

#include <stdio.h>
#include <string.h>

#define TILE_H      44
#define TILE_GAP    5
#define TILE_W      111                 /* two per row: 6 + 111 + 6 + 111 + 6 */
#define BTN_ROW_Y   266
#define SUB_ROW_Y   202                 /* no BLF keys: Contacts | History */
#define SUB_ROW_H   46
#define MAIN_ROW_Y  252                 /* no BLF keys: Dial | Settings */
#define MAIN_ROW_H  62
#define BTN_ROW_H   48
#define GRID_BOTTOM (BTN_ROW_Y - 6)
#define LAMP_D      12
#define TILE_TXT_X  26                  /* text left of the lamp */
#define BLINK_MS    450

typedef struct {
    lv_obj_t *btn;
    lv_obj_t *lamp;
    lv_obj_t *name;                     /* fit label: label, or ext */
    lv_obj_t *ext;                      /* small ext under a label */
    char ext_txt[UI_BLF_EXT_LEN];
    ui_blf_state_t state;
} tile_t;

typedef struct {
    lv_obj_t *crow;
    lv_obj_t *clock;
    lv_obj_t *ampm;             /* "AM"/"PM" next to the big clock; hidden in 24 h mode */
    lv_obj_t *name;
    lv_obj_t *id;
    lv_obj_t *state;
    lv_obj_t *sub;
    char sub_txt[UI_NAME_LEN + 32];     /* what `sub` (DOTS mode) was last set to */
    bool sub_valid;
    lv_obj_t *mwi_btn;          /* "N new voicemails" (no BLF keys) */
    lv_obj_t *mwi_lbl;          /* envelope + "N new", 24 pt */
    lv_obj_t *mwi_lbl2;         /* "voicemails", 14 pt */
    lv_obj_t *dial;
    lv_obj_t *con;              /* Contacts */
    lv_obj_t *hist;             /* History (+ missed badge) */
    lv_obj_t *badge;
    lv_obj_t *badge_lbl;
    lv_obj_t *set;
    lv_obj_t *vm;               /* voicemail button in the bottom row (BLF keys) */
    lv_obj_t *vm_cap;
    lv_obj_t *scr;
    tile_t tile[UI_BLF_MAX];
    int n_tiles;                /* created so far */
    lv_timer_t *blink;
    bool blink_on;
    /* layout key: re-layout only when it changes */
    int lay_n;
    bool lay_mwi;
    bool compact;               /* 4 rows: one-line header */
} home_t;

static home_t *s_h;

static void on_delete(lv_event_t *e)
{
    home_t *h = lv_event_get_user_data(e);
    if (s_h == h) {
        s_h = NULL;
    }
    if (h->blink) {
        lv_timer_delete(h->blink);
    }
    lv_free(h);
}

static void on_dial(lv_event_t *e)
{
    (void)e;
    ui_goto(UI_SCR_DIALER, NULL);
}

static void on_contacts(lv_event_t *e)
{
    (void)e;
    ui_goto(UI_SCR_CONTACTS, NULL);
}

static void on_history(lv_event_t *e)
{
    (void)e;
    ui_goto(UI_SCR_HISTORY, NULL);
}

static void on_settings(lv_event_t *e)
{
    (void)e;
    ui_goto(UI_SCR_SETTINGS, NULL);
}

static void on_voicemail(lv_event_t *e)
{
    (void)e;
    const char *vm = ui_model()->vm_number;
    if (!vm[0]) {
        ui_toast("No voicemail number set");
        return;
    }
    ui_act_dial(vm);
}

static void on_tile(lv_event_t *e)
{
    home_t *h = s_h;
    intptr_t i = (intptr_t)lv_event_get_user_data(e);
    if (h && i >= 0 && i < UI_BLF_MAX && h->tile[i].ext_txt[0]) {
        ui_act_dial(h->tile[i].ext_txt);
    }
}

static lv_color_t lamp_color(ui_blf_state_t st, bool blink_on)
{
    switch (st) {
    case UI_BLF_IDLE:    return UI_COL_OK;
    case UI_BLF_BUSY:    return UI_COL_RED;
    case UI_BLF_RINGING: return blink_on ? UI_COL_RED : UI_COL_WARN;
    case UI_BLF_UNKNOWN:
    default:             return UI_COL_SURFACE2;
    }
}

static void tile_paint(home_t *h, tile_t *t)
{
    lv_color_t c = lamp_color(t->state, h->blink_on);
    ui_style_set_color(t->lamp, LV_STYLE_BG_COLOR, c, 0);
    /* a ringing tile is outlined as well, so it catches the eye */
    ui_style_set_num(t->btn, LV_STYLE_BORDER_WIDTH, t->state == UI_BLF_RINGING ? 2 : 0, 0);
    ui_style_set_color(t->btn, LV_STYLE_BORDER_COLOR, c, 0);
}

/* Runs only while a tile rings (tile_update resumes it): no idle LVGL wake-ups otherwise. */
static void blink_cb(lv_timer_t *timer)
{
    home_t *h = lv_timer_get_user_data(timer);
    h->blink_on = !h->blink_on;
    bool ringing = false;
    for (int i = 0; i < h->lay_n; i++) {
        if (h->tile[i].state == UI_BLF_RINGING) {
            tile_paint(h, &h->tile[i]);
            ringing = true;
        }
    }
    if (!ringing) {
        lv_timer_pause(timer);
    }
}

static void tile_create(home_t *h, lv_obj_t *scr, int i)
{
    tile_t *t = &h->tile[i];
    t->btn = ui_btn_create(scr, UI_BTN_NEUTRAL, NULL, 0, 0, TILE_W, TILE_H);
    lv_obj_set_style_border_side(t->btn, LV_BORDER_SIDE_FULL, 0);
    lv_obj_add_event_cb(t->btn, on_tile, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    t->lamp = ui_box_create(t->btn, 8, (TILE_H - LAMP_D) / 2, LAMP_D, LAMP_D);
    lv_obj_set_style_radius(t->lamp, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(t->lamp, LV_OPA_COVER, 0);
    t->name = ui_fit_create(t->btn, TILE_TXT_X, 0, TILE_W - TILE_TXT_X - 5, 26, UI_COL_TEXT);
    lv_obj_set_style_text_align(lv_obj_get_child(t->name, 0), LV_TEXT_ALIGN_LEFT, 0);
    t->ext = ui_label_create(t->btn, &lv_font_montserrat_14, UI_COL_DIM, "");
    lv_obj_set_pos(t->ext, TILE_TXT_X, 25);
    lv_obj_set_hidden(t->btn, true);
}

static void tile_set_width(tile_t *t, int32_t w)
{
    lv_obj_set_width(t->btn, w);
    lv_obj_set_width(t->name, w - TILE_TXT_X - 5);
    lv_obj_set_width(lv_obj_get_child(t->name, 0), w - TILE_TXT_X - 5);
}

static lv_obj_t *home_create(const char *arg)
{
    (void)arg;
    home_t *h = lv_zalloc(sizeof(*h));
    if (!h) {
        return NULL;
    }
    lv_obj_t *scr = ui_screen_create();
    lv_obj_add_event_cb(scr, on_delete, LV_EVENT_DELETE, h);
    ui_statusbar_create(scr);

    /* "3:07" in 40 pt with a 24 pt "PM" beside it, the pair centred as one. */
    h->crow = ui_box_create(scr, 0, 44, UI_W, 48);
    lv_obj_set_flex_flow(h->crow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(h->crow, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(h->crow, 6, 0);
    h->clock = ui_label_create(h->crow, &lv_font_montserrat_40, UI_COL_TEXT, "");
    h->ampm = ui_label_create(h->crow, &lv_font_montserrat_24, UI_COL_DIM, "");
    lv_obj_set_style_translate_y(h->ampm, -3, 0);
    lv_obj_set_hidden(h->ampm, true);

    h->name = ui_fit_create(scr, 6, 108, 228, 34, UI_COL_TEXT);
    h->id = ui_fit_create(scr, 6, 142, 228, 20, UI_COL_DIM);

    h->state = lv_label_create(scr);
    lv_obj_set_style_text_font(h->state, &lv_font_montserrat_24, 0);
    lv_obj_set_width(h->state, 216);
    lv_obj_set_style_text_align(h->state, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(h->state, 12, 176);

    h->sub = ui_label_create(scr, &lv_font_montserrat_14, UI_COL_BAD, "");
    lv_obj_set_width(h->sub, 216);
    lv_obj_set_height(h->sub, 18);      /* one line: DOTS needs a fixed height */
    lv_label_set_long_mode(h->sub, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(h->sub, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(h->sub, 12, 212);
    lv_obj_set_ext_click_area(h->sub, 8);
    lv_obj_add_event_cb(h->sub, on_history, LV_EVENT_CLICKED, NULL);   /* only while it shows missed calls */

    h->scr = scr;                       /* tiles are created when first needed */

    /* "[envelope] 3 new" in 24 pt + "voicemails" in 14 pt, centred as one */
    h->mwi_btn = ui_btn_create(scr, UI_BTN_NEUTRAL, NULL, 12, 198, 216, 44);
    lv_obj_set_style_border_width(h->mwi_btn, 2, 0);
    lv_obj_set_style_border_color(h->mwi_btn, UI_COL_WARN, 0);
    lv_obj_set_flex_flow(h->mwi_btn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(h->mwi_btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(h->mwi_btn, 6, 0);
    h->mwi_lbl = ui_label_create(h->mwi_btn, &lv_font_montserrat_24, UI_COL_WARN, "");
    h->mwi_lbl2 = ui_label_create(h->mwi_btn, &lv_font_montserrat_14, UI_COL_WARN, "");
    lv_obj_set_style_translate_y(h->mwi_lbl2, -3, 0);
    lv_obj_add_event_cb(h->mwi_btn, on_voicemail, LV_EVENT_CLICKED, NULL);
    lv_obj_set_hidden(h->mwi_btn, true);

    h->dial = ui_icon_btn_create(scr, UI_BTN_GREEN, LV_SYMBOL_CALL, &lv_font_montserrat_24, 0, "Dial",
                                 &lv_font_montserrat_14, 6, MAIN_ROW_Y, 112, MAIN_ROW_H);
    lv_obj_add_event_cb(h->dial, on_dial, LV_EVENT_CLICKED, NULL);
    h->con = ui_icon_btn_create(scr, UI_BTN_NEUTRAL, LV_SYMBOL_DIRECTORY, &lv_font_montserrat_24, 0,
                                "Contacts", &lv_font_montserrat_14, 6, SUB_ROW_Y, 112, SUB_ROW_H);
    lv_obj_add_event_cb(h->con, on_contacts, LV_EVENT_CLICKED, NULL);
    h->hist = ui_icon_btn_create(scr, UI_BTN_NEUTRAL, LV_SYMBOL_LIST, &lv_font_montserrat_24, 0,
                                 "History", &lv_font_montserrat_14, 122, SUB_ROW_Y, 112, SUB_ROW_H);
    lv_obj_add_event_cb(h->hist, on_history, LV_EVENT_CLICKED, NULL);
    h->badge = ui_box_create(h->hist, 0, 0, 22, 20);
    lv_obj_set_floating(h->badge, true);
    lv_obj_align(h->badge, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(h->badge, UI_COL_RED, 0);
    lv_obj_set_style_bg_opa(h->badge, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(h->badge, 10, 0);
    h->badge_lbl = ui_label_create(h->badge, &lv_font_montserrat_14, UI_COL_TEXT, "");
    lv_obj_center(h->badge_lbl);
    lv_obj_set_hidden(h->badge, true);
    h->set = ui_icon_btn_create(scr, UI_BTN_NEUTRAL, LV_SYMBOL_SETTINGS, &lv_font_montserrat_24, 0,
                                "Settings", &lv_font_montserrat_14, 122, MAIN_ROW_Y, 112, MAIN_ROW_H);
    lv_obj_add_event_cb(h->set, on_settings, LV_EVENT_CLICKED, NULL);
    h->vm = ui_icon_btn_create(scr, UI_BTN_NEUTRAL, LV_SYMBOL_ENVELOPE, &lv_font_montserrat_24, 0, "",
                               &lv_font_montserrat_14, 84, BTN_ROW_Y, 72, BTN_ROW_H);
    lv_obj_set_style_text_color(lv_obj_get_child(h->vm, 0), UI_COL_WARN, 0);
    h->vm_cap = lv_obj_get_child(h->vm, 1);
    lv_obj_add_event_cb(h->vm, on_voicemail, LV_EVENT_CLICKED, NULL);
    lv_obj_set_hidden(h->vm, true);

    h->lay_n = -1;
    h->blink = lv_timer_create(blink_cb, BLINK_MS, h);
    s_h = h;
    return scr;
}

/* "Display Name <user@server>" -> name + id; a bare "user" is the name. */
static void split_account(const char *acc, char *name, size_t ncap, char *id, size_t icap)
{
    name[0] = id[0] = '\0';
    const char *lt = strchr(acc, '<');
    const char *gt = lt ? strchr(lt, '>') : NULL;
    if (lt && gt) {
        size_t n = (size_t)(lt - acc);
        while (n > 0 && acc[n - 1] == ' ') {
            n--;
        }
        snprintf(name, ncap, "%.*s", (int)n, acc);
        snprintf(id, icap, "%.*s", (int)(gt - lt - 1), lt + 1);
        if (!name[0]) {
            snprintf(name, ncap, "%s", id);
            id[0] = '\0';
        }
    } else {
        snprintf(name, ncap, "%s", acc);
    }
}

static void set_y(lv_obj_t *o, int32_t y, bool show)
{
    lv_obj_set_hidden(o, !show);
    lv_obj_set_y(o, y);
}

/* BLF mode bottom row: Dial, Contacts, History, [Voicemail], Settings, icon-only (the
 * captions would not fit 4 or 5 buttons). */
static void place_row(home_t *h, int32_t y, int32_t bh, bool vm)
{
    lv_obj_t *b[5] = {h->dial, h->con, h->hist, h->vm, h->set};
    int cnt = vm ? 5 : 4;
    int32_t gap = 4, w = (UI_W - 12 - gap * (cnt - 1)) / cnt;
    int32_t x = 6;
    for (int i = 0; i < 5; i++) {
        if (i == 3 && !vm) {
            lv_obj_set_hidden(h->vm, true);
            continue;
        }
        lv_obj_set_pos(b[i], x, y);
        lv_obj_set_size(b[i], w, bh);
        if (i != 3) {
            lv_obj_set_hidden(lv_obj_get_child(b[i], 1), true);   /* caption */
        }
        lv_obj_set_hidden(b[i], false);
        x += w + gap;
    }
}

/* No BLF keys: Contacts | History above Dial | Settings with their captions. */
static void place_wide(home_t *h)
{
    lv_obj_t *b[4] = {h->con, h->hist, h->dial, h->set};
    const int32_t y[4] = {SUB_ROW_Y, SUB_ROW_Y, MAIN_ROW_Y, MAIN_ROW_Y};
    const int32_t bh[4] = {SUB_ROW_H, SUB_ROW_H, MAIN_ROW_H, MAIN_ROW_H};
    for (int i = 0; i < 4; i++) {
        lv_obj_set_pos(b[i], (i % 2) ? 122 : 6, y[i]);
        lv_obj_set_size(b[i], 112, bh[i]);
        lv_obj_set_hidden(lv_obj_get_child(b[i], 1), false);
        lv_obj_set_hidden(b[i], false);
    }
    lv_obj_set_hidden(h->vm, true);
}

/* Place everything for `n` BLF keys and the voicemail button. */
static void home_layout(home_t *h, int n, bool mwi)
{
    h->lay_n = n;
    h->lay_mwi = mwi;
    for (; h->n_tiles < n; h->n_tiles++) {
        tile_create(h, h->scr, h->n_tiles);
    }
    int rows = (n + 1) / 2;
    h->compact = rows >= 4;

    /* header */
    lv_obj_set_flex_align(h->crow, h->compact ? LV_FLEX_ALIGN_START : LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_x(h->crow, h->compact ? 8 : 0);
    lv_obj_set_size(h->crow, h->compact ? 140 : UI_W, h->compact ? 40 : 48);
    lv_obj_set_style_text_font(h->clock, h->compact ? &lv_font_montserrat_32 : &lv_font_montserrat_40, 0);
    lv_obj_set_style_text_font(h->ampm, h->compact ? &lv_font_montserrat_14 : &lv_font_montserrat_24, 0);
    lv_obj_set_style_translate_y(h->ampm, h->compact ? -5 : -3, 0);
    lv_obj_set_style_text_font(h->state, h->compact ? &lv_font_montserrat_14 : &lv_font_montserrat_24, 0);
    lv_obj_set_width(h->state, h->compact ? 92 : 216);
    lv_obj_set_x(h->state, h->compact ? 140 : 12);
    lv_obj_set_style_text_align(h->state, h->compact ? LV_TEXT_ALIGN_RIGHT : LV_TEXT_ALIGN_CENTER, 0);

    if (n == 0) {
        if (!mwi) {
            set_y(h->crow, 28, true);
            set_y(h->name, 80, true);
            set_y(h->id, 114, true);
            set_y(h->state, 138, true);
            set_y(h->sub, 170, true);
        } else {
            set_y(h->crow, 26, true);
            set_y(h->name, 76, true);
            set_y(h->id, 0, false);
            set_y(h->state, 112, true);
            set_y(h->sub, 0, false);
        }
        lv_obj_set_pos(h->mwi_btn, 12, 148);
        lv_obj_set_size(h->mwi_btn, 216, 46);
        lv_obj_set_hidden(h->mwi_btn, !mwi);
        place_wide(h);
    } else {
        int32_t top = GRID_BOTTOM - (rows * TILE_H + (rows - 1) * TILE_GAP);
        switch (rows) {
        case 1: /* top 216 */
            set_y(h->crow, 34, true);
            set_y(h->name, 92, true);
            set_y(h->id, 126, true);
            set_y(h->state, 154, true);
            set_y(h->sub, 186, true);
            break;
        case 2: /* top 167 */
            set_y(h->crow, 28, true);
            set_y(h->name, 82, true);
            set_y(h->id, 0, false);
            set_y(h->state, 124, true);
            set_y(h->sub, 0, false);
            break;
        case 3: /* top 118 */
            set_y(h->crow, 26, true);
            set_y(h->name, 0, false);
            set_y(h->id, 0, false);
            set_y(h->state, 80, true);
            set_y(h->sub, 0, false);
            break;
        default: /* 4 rows, top 69: one line */
            set_y(h->crow, 24, true);
            set_y(h->name, 0, false);
            set_y(h->id, 0, false);
            set_y(h->state, 37, true);
            set_y(h->sub, 0, false);
            break;
        }
        for (int i = 0; i < n; i++) {
            tile_t *t = &h->tile[i];
            bool wide = (i == n - 1) && (n % 2 == 1);
            int32_t x = (i % 2 == 0) ? 6 : 6 + TILE_W + 6;
            int32_t y = top + (i / 2) * (TILE_H + TILE_GAP);
            lv_obj_set_pos(t->btn, x, y);
            tile_set_width(t, wide ? 2 * TILE_W + 6 : TILE_W);
        }
        lv_obj_set_hidden(h->mwi_btn, true);
        place_row(h, BTN_ROW_Y, BTN_ROW_H, mwi);
    }
    for (int i = 0; i < h->n_tiles; i++) {
        lv_obj_set_hidden(h->tile[i].btn, i >= n);
    }
}

static void tile_update(home_t *h, tile_t *t, const ui_blf_t *b)
{
    snprintf(t->ext_txt, sizeof(t->ext_txt), "%s", b->ext);
    if (b->label[0]) {
        /* label on top (24 pt, smaller if long), the extension under it */
        lv_obj_set_y(t->name, 1);
        lv_obj_set_height(t->name, 26);
        ui_fit_set(t->name, b->label, &lv_font_montserrat_24, false);
        ui_label_set_text_if_changed(t->ext, b->ext);
        lv_obj_set_hidden(t->ext, false);
    } else {
        lv_obj_set_y(t->name, (TILE_H - 30) / 2);
        lv_obj_set_height(t->name, 30);
        ui_fit_set(t->name, b->ext, &lv_font_montserrat_24, true);
        lv_obj_set_hidden(t->ext, true);
    }
    t->state = b->state;
    tile_paint(h, t);
    if (t->state == UI_BLF_RINGING && h->blink && lv_timer_get_paused(h->blink)) {
        lv_timer_resume(h->blink);
    }
}

static void home_update(const ui_model_t *m)
{
    home_t *h = s_h;
    if (!h) {
        return;
    }
    int n = m->blf_count > UI_BLF_MAX ? UI_BLF_MAX : m->blf_count;
    if (n != h->lay_n || m->mwi_waiting != h->lay_mwi) {
        home_layout(h, n, m->mwi_waiting);
    }

    if (m->time_hhmm[0]) {
        /* "3:07 PM" -> "3:07" + "PM"; 24 h mode has no suffix */
        const char *sp = strchr(m->time_hhmm, ' ');
        char hm[UI_TIME_STR_LEN];
        snprintf(hm, sizeof(hm), "%.*s", sp ? (int)(sp - m->time_hhmm) : (int)sizeof(hm) - 1,
                 m->time_hhmm);
        ui_label_set_text_if_changed(h->clock, hm);
        ui_set_text_color(h->clock, UI_COL_TEXT);
        ui_label_set_text_if_changed(h->ampm, sp ? sp + 1 : "");
        lv_obj_set_hidden(h->ampm, sp == NULL);
    } else {
        ui_label_set_text_if_changed(h->clock, "sipwire");
        ui_set_text_color(h->clock, UI_COL_DIM);
        lv_obj_set_hidden(h->ampm, true);
    }

    char name[64], id[64];
    split_account(m->account, name, sizeof(name), id, sizeof(id));
    ui_fit_set(h->name, name[0] ? name : "No account", &lv_font_montserrat_24, false);
    ui_fit_set_color(h->name, name[0] ? UI_COL_TEXT : UI_COL_DIM);
    ui_fit_set(h->id, id, &lv_font_montserrat_14, false);

    const char *icon;
    const char *txt;
    const char *shrt;
    const char *sub = "";
    lv_color_t col;
    if (m->net == UI_NET_OFF) {
        icon = LV_SYMBOL_CLOSE;
        txt = "Offline";
        shrt = "No Wi-Fi";
        sub = "No Wi-Fi";
        col = UI_COL_BAD;
    } else {
        switch (m->reg) {
        case UI_REG_REGISTERED:
            icon = LV_SYMBOL_OK;
            txt = shrt = "Ready";
            col = UI_COL_OK;
            break;
        case UI_REG_REGISTERING:
            icon = LV_SYMBOL_REFRESH;
            txt = "Registering...";
            shrt = "Registering";
            col = UI_COL_WARN;
            break;
        case UI_REG_FAILED:
            icon = LV_SYMBOL_WARNING;
            txt = "Registration failed";
            shrt = "Reg. failed";
            col = UI_COL_BAD;
            break;
        default:
            icon = LV_SYMBOL_CLOSE;
            txt = shrt = "Offline";
            col = UI_COL_DIM;
            break;
        }
        if (m->net == UI_NET_CONNECTING) {
            sub = "Connecting to Wi-Fi...";
        }
    }
    char buf[sizeof(h->sub_txt)];
    snprintf(buf, sizeof(buf), "%s %s", icon, h->compact ? shrt : txt);
    ui_label_set_text_if_changed(h->state, buf);
    ui_set_text_color(h->state, col);
    bool missed_note = !sub[0] && m->missed_unseen > 0 && m->missed_last[0];
    if (missed_note) {
        if (m->missed_unseen == 1) {
            snprintf(buf, sizeof(buf), LV_SYMBOL_CALL " Missed: %s", m->missed_last);
        } else {
            snprintf(buf, sizeof(buf), LV_SYMBOL_CALL " Missed (%u): %s", (unsigned)m->missed_unseen,
                     m->missed_last);
        }
    } else {
        snprintf(buf, sizeof(buf), "%s", sub);
    }
    /* DOTS mode: compare with our copy, LVGL's buffer may end in "..." */
    if (!h->sub_valid || strcmp(buf, h->sub_txt) != 0) {
        lv_label_set_text(h->sub, buf);
        memcpy(h->sub_txt, buf, sizeof(buf));
        h->sub_valid = true;
    }
    lv_obj_set_clickable(h->sub, missed_note);

    for (int i = 0; i < n; i++) {
        tile_update(h, &h->tile[i], &m->blf[i]);
    }

    /* missed-call badge on History */
    if (m->missed_unseen > 0) {
        if (m->missed_unseen > 9) {
            snprintf(buf, sizeof(buf), "9+");
        } else {
            snprintf(buf, sizeof(buf), "%u", (unsigned)m->missed_unseen);
        }
        ui_label_set_text_if_changed(h->badge_lbl, buf);
        lv_obj_set_width(h->badge, m->missed_unseen > 9 ? 26 : 20);
        ui_style_set_num(h->badge, LV_STYLE_ALIGN, LV_ALIGN_TOP_RIGHT, 0);     /* = lv_obj_align(.., 0, 0) */
        lv_obj_set_pos(h->badge, 0, 0);
        lv_obj_set_hidden(h->badge, false);
        ui_set_text_color(lv_obj_get_child(h->hist, 0), UI_COL_BAD);
    } else {
        lv_obj_set_hidden(h->badge, true);
        ui_set_text_color(lv_obj_get_child(h->hist, 0), UI_COL_TEXT);
    }

    if (m->mwi_waiting) {
        if (m->mwi_new > 0) {
            unsigned nn = m->mwi_new > 999 ? 999u : (unsigned)m->mwi_new;
            snprintf(buf, sizeof(buf), LV_SYMBOL_ENVELOPE " %u new", nn);
            ui_label_set_text_if_changed(h->mwi_lbl, buf);
            ui_label_set_text_if_changed(h->mwi_lbl2, nn == 1 ? "voicemail" : "voicemails");
            snprintf(buf, sizeof(buf), "%u new", nn);
            ui_label_set_text_if_changed(h->vm_cap, buf);
        } else {
            ui_label_set_text_if_changed(h->mwi_lbl, LV_SYMBOL_ENVELOPE " Voicemail");
            ui_label_set_text_if_changed(h->mwi_lbl2, "");
            ui_label_set_text_if_changed(h->vm_cap, "Voicemail");
        }
    }
}

static void home_key(char key, ui_key_action_t action)
{
    if (action == UI_KEY_PRESS && ui_is_dial_key(key)) {
        char first[2] = {key, '\0'};
        ui_goto(UI_SCR_DIALER, first);
    } else if (action == UI_KEY_LONG && key == '0') {
        ui_goto(UI_SCR_DIALER, "+");
    }
}

const ui_screen_ops_t ui_ops_home = {home_create, home_update, home_key, NULL, NULL};
