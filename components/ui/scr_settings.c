/* Settings: scrolling list -> Wi-Fi / SIP account / BLF keys / Time / Update
 * forms (on-screen keyboard) / About / touch calibration screen. The forms are prefilled from the model when opened
 * (passwords are never in the model: an empty field keeps the stored one).
 * All pages live in one screen and are switched by hiding; the keyboard is
 * shared and only shown while a field is being edited. */

#include "ui_priv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BODY_Y (UI_BAR_H + UI_HEADER_H)
#define BODY_H (UI_H - BODY_Y)
#define KB_H 168
#define FIELD_H 44
#define DEFAULT_SIP_PORT 5060
#define OTA_URL_MAX_CHARS 191       /* ota.h OTA_URL_MAX - 1 */

typedef enum {
    PG_LIST, PG_WIFI, PG_ACCOUNT, PG_BLF, PG_TIME, PG_UPDATE, PG_ABOUT,
    PG_SOUNDS, PG_FEATURES,     /* no keyboard, apply immediately */
    PG_COUNT
} page_t;

#define TONE_MAX 12             /* ringtone rows built at most */
#define SLIDER_ROWS 3           /* ring, call, speaker volume */

/* Dial pad for extensions / the voicemail number (LVGL's number pad has no
 * '*' or '#'); "abc" switches to letters for alphanumeric extensions. */
static const char *const DIAL_KB_MAP[] = {"1", "2", "3", LV_SYMBOL_BACKSPACE, "\n",
                                          "4", "5", "6", LV_SYMBOL_OK, "\n",
                                          "7", "8", "9", "abc", "\n",
                                          "*", "0", "#", LV_SYMBOL_LEFT, LV_SYMBOL_RIGHT, ""};
static const lv_buttonmatrix_ctrl_t DIAL_KB_CTRL[] = {
    1, 1, 1, LV_KEYBOARD_CTRL_BUTTON_FLAGS | 2,
    1, 1, 1, LV_KEYBOARD_CTRL_BUTTON_FLAGS | 2,
    1, 1, 1, LV_KEYBOARD_CTRL_BUTTON_FLAGS | 2,
    1, 1, 1, 1, 1};

/* Time zones offered in Settings > Time: friendly name -> POSIX TZ string. The last
 * entry, "Custom", shows a text field for a raw POSIX TZ string instead. */
#define ZONES(X)                                           \
    X("UTC", "UTC0")                                        \
    X("US Eastern", "EST5EDT,M3.2.0,M11.1.0")               \
    X("US Central", "CST6CDT,M3.2.0,M11.1.0")               \
    X("US Mountain", "MST7MDT,M3.2.0,M11.1.0")              \
    X("US Pacific", "PST8PDT,M3.2.0,M11.1.0")               \
    X("US Alaska", "AKST9AKDT,M3.2.0,M11.1.0")              \
    X("Hawaii", "HST10")                                    \
    X("UK", "GMT0BST,M3.5.0/1,M10.5.0")                     \
    X("Central Europe", "CET-1CEST,M3.5.0,M10.5.0/3")       \
    X("Eastern Europe", "EET-2EEST,M3.5.0/3,M10.5.0/4")     \
    X("India", "IST-5:30")                                  \
    X("China", "CST-8")                                     \
    X("Japan", "JST-9")                                     \
    X("Australia East", "AEST-10AEDT,M10.1.0,M4.1.0/3")     \
    X("New Zealand", "NZST-12NZDT,M9.5.0,M4.1.0/3")

typedef struct {
    const char *name;
    const char *posix;          /* NULL = Custom */
} zone_t;

#define ZONE_ENTRY(n, p) {n, p},
#define ZONE_NAME(n, p) n "\n"
static const zone_t ZONE[] = {ZONES(ZONE_ENTRY){"Custom", NULL}};
static const char ZONE_OPTIONS[] = ZONES(ZONE_NAME) "Custom";
#define ZONE_COUNT ((uint32_t)(sizeof(ZONE) / sizeof(ZONE[0])))
#define ZONE_CUSTOM (ZONE_COUNT - 1)

typedef struct {
    lv_obj_t *title;
    lv_obj_t *save;
    lv_obj_t *page[PG_COUNT];
    lv_obj_t *kb;
    page_t cur;

    lv_obj_t *ssid, *wpass;
    lv_obj_t *server, *port, *user, *apass, *dname;
    lv_obj_t *ntp, *tz_dd, *tz_custom, *h24;
    uint32_t tz_prev;               /* dropdown index before the last change */
    lv_obj_t *fw, *ota_url, *ota_btn, *ota_bar, *ota_msg;

    /* BLF keys page: the entries being edited (saved as a whole) */
    lv_obj_t *blf_cap, *blf_rows, *blf_ext, *blf_label, *blf_add, *vm;
    ui_blf_t blf[UI_BLF_MAX];
    int blf_n;

    lv_obj_t *about_ip, *about_acc, *about_reg, *about_wifi, *about_fw;

    /* Sounds page */
    lv_obj_t *tone_box;
    lv_obj_t *tone_row[TONE_MAX];
    int tone_n;
    int tone_sel;                   /* selected ringtone */
    int tone_playing;               /* preview running (-1 = none) */
    lv_obj_t *vol[SLIDER_ROWS];     /* sliders: ring, call, speaker */
    lv_obj_t *vol_cap[SLIDER_ROWS];

    /* Call features page */
    lv_obj_t *sw_cw, *sw_aa, *sw_dnd;
    lv_obj_t *sw_quiet, *sw_qallow, *dd_qstart, *dd_qend;
    uint16_t q_start, q_end;        /* minutes; a dropdown only replaces its own on change */
} settings_t;

static settings_t *s_s;

static const char *const TITLES[PG_COUNT] = {"Settings", "Wi-Fi", "Account", "BLF", "Time", "Update",
                                             "About", "Sounds", "Call features"};

static void on_delete(lv_event_t *e)
{
    settings_t *s = lv_event_get_user_data(e);
    if (s_s == s) {
        s_s = NULL;
    }
    lv_free(s);
}

/* ---- keyboard ------------------------------------------------------------ */

static bool is_form(page_t p)
{
    return p == PG_WIFI || p == PG_ACCOUNT || p == PG_BLF || p == PG_TIME || p == PG_UPDATE;
}

static void kb_hide(settings_t *s)
{
    lv_keyboard_set_textarea(s->kb, NULL);
    lv_obj_set_hidden(s->kb, true);
    for (page_t p = PG_WIFI; p <= PG_UPDATE; p++) {
        lv_obj_set_height(s->page[p], BODY_H);
    }
}

static void kb_event(lv_event_t *e)
{
    kb_hide(lv_event_get_user_data(e));
}

static void field_clicked(lv_event_t *e)
{
    settings_t *s = lv_event_get_user_data(e);
    lv_obj_t *ta = lv_event_get_target_obj(e);
    lv_keyboard_set_mode(s->kb, ta == s->port                        ? LV_KEYBOARD_MODE_NUMBER
                                : (ta == s->blf_ext || ta == s->vm) ? LV_KEYBOARD_MODE_USER_1
                                                                    : LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_keyboard_set_textarea(s->kb, ta);
    lv_obj_set_hidden(s->kb, false);
    lv_obj_set_height(s->page[s->cur], BODY_H - KB_H);
    lv_obj_update_layout(s->page[s->cur]);
    lv_obj_scroll_to_view(lv_obj_get_parent(ta), LV_ANIM_OFF);
}

static void form_clicked(lv_event_t *e)
{
    kb_hide(lv_event_get_user_data(e)); /* tap on the empty form area */
}

static void eye_clicked(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target_obj(e);
    lv_obj_t *ta = lv_event_get_user_data(e);
    bool hide = !lv_textarea_get_password_mode(ta);
    lv_textarea_set_password_mode(ta, hide);
    lv_label_set_text(lv_obj_get_child(btn, 0), hide ? LV_SYMBOL_EYE_CLOSE : LV_SYMBOL_EYE_OPEN);
}

/* ---- pages ----------------------------------------------------------------- */

/* Password fields never hold the stored secret: "unchanged" when one is set,
 * an amber "required" otherwise. */
static void set_pass_placeholder(lv_obj_t *ta, bool is_set, const char *required)
{
    lv_textarea_set_placeholder_text(ta, is_set ? "unchanged" : required);
    lv_obj_set_style_text_color(ta, is_set ? lv_color_hex(0x7D8996) : UI_COL_WARN,
                                LV_PART_TEXTAREA_PLACEHOLDER);
}

/* Show or hide a field made by field_create (caption + row). */
static void field_set_hidden(lv_obj_t *ta, bool hidden)
{
    lv_obj_t *row = lv_obj_get_parent(ta);
    lv_obj_t *cap = lv_obj_get_child(lv_obj_get_parent(row), (int32_t)lv_obj_get_index(row) - 1);
    lv_obj_set_hidden(row, hidden);
    lv_obj_set_hidden(cap, hidden);
}

static void tz_select(settings_t *s, uint32_t idx)
{
    lv_dropdown_set_selected(s->tz_dd, idx);
    s->tz_prev = idx;
    field_set_hidden(s->tz_custom, idx != ZONE_CUSTOM);
}

static void prefill_time(settings_t *s, const ui_model_t *m)
{
    uint32_t idx = ZONE_CUSTOM;
    for (uint32_t i = 0; i < ZONE_CUSTOM; i++) {
        if (strcmp(m->tz, ZONE[i].posix) == 0) {
            idx = i;
            break;
        }
    }
    lv_textarea_set_text(s->ntp, m->ntp_server);
    lv_textarea_set_text(s->tz_custom, m->tz);
    lv_obj_update_layout(s->tz_custom);
    lv_textarea_set_cursor_pos(s->tz_custom, 0);
    lv_obj_scroll_to_x(s->tz_custom, 0, LV_ANIM_OFF);
    tz_select(s, idx);
    if (m->clock_24h) {
        lv_obj_add_state(s->h24, LV_STATE_CHECKED);
    } else {
        lv_obj_remove_state(s->h24, LV_STATE_CHECKED);
    }
}

static void update_ota(settings_t *s, const ui_model_t *m);
static void blf_rebuild(settings_t *s);

static void prefill(settings_t *s, page_t p)
{
    const ui_model_t *m = ui_model();
    if (p == PG_BLF) {
        s->blf_n = m->blf_count > UI_BLF_MAX ? UI_BLF_MAX : m->blf_count;
        memcpy(s->blf, m->blf, sizeof(s->blf));
        lv_textarea_set_text(s->blf_ext, "");
        lv_textarea_set_text(s->blf_label, "");
        lv_textarea_set_text(s->vm, m->vm_number);
        blf_rebuild(s);
    } else if (p == PG_TIME) {
        prefill_time(s, m);
    } else if (p == PG_UPDATE) {
        lv_textarea_set_text(s->ota_url, m->ota_url);
        lv_obj_update_layout(s->ota_url);
        lv_textarea_set_cursor_pos(s->ota_url, 0);   /* show the start of the URL, not its end */
        lv_obj_scroll_to_x(s->ota_url, 0, LV_ANIM_OFF);
        update_ota(s, m);
    } else if (p == PG_WIFI) {
        lv_textarea_set_text(s->ssid, m->wifi_ssid);
        lv_textarea_set_text(s->wpass, "");
        set_pass_placeholder(s->wpass, m->wifi_pass_set, "required");
    } else if (p == PG_ACCOUNT) {
        char port[8] = "";
        if (m->sip_port) {
            snprintf(port, sizeof(port), "%u", (unsigned)m->sip_port);
        }
        lv_textarea_set_text(s->server, m->sip_server);
        lv_textarea_set_text(s->port, port);
        lv_textarea_set_text(s->user, m->sip_user);
        lv_textarea_set_text(s->apass, "");
        lv_textarea_set_text(s->dname, m->sip_display_name);
        set_pass_placeholder(s->apass, m->sip_pass_set, "required");
    }
}

static void sounds_stop(settings_t *s);
static void sounds_prefill(settings_t *s);
static void features_prefill(settings_t *s);

static void show_page(settings_t *s, page_t p)
{
    kb_hide(s);
    sounds_stop(s);                 /* a preview never outlives its page */
    for (int i = 0; i < PG_COUNT; i++) {
        if (i == (int)p) {
            lv_obj_set_hidden(s->page[i], false);
        } else {
            lv_obj_set_hidden(s->page[i], true);
        }
    }
    s->cur = p;
    lv_label_set_text(s->title, TITLES[p]);
    if (is_form(p)) {
        prefill(s, p);
        lv_obj_set_hidden(s->save, p == PG_UPDATE);   /* Update has its own button */
        lv_obj_scroll_to_y(s->page[p], 0, LV_ANIM_OFF);
    } else {
        lv_obj_set_hidden(s->save, true);
        if (p == PG_SOUNDS) {
            sounds_prefill(s);
            lv_obj_scroll_to_y(s->page[p], 0, LV_ANIM_OFF);
        } else if (p == PG_FEATURES) {
            features_prefill(s);
        }
    }
}

static void on_back(lv_event_t *e)
{
    settings_t *s = lv_event_get_user_data(e);
    if (s->cur == PG_LIST) {
        ui_goto(UI_SCR_HOME, NULL);
    } else {
        show_page(s, PG_LIST);
    }
}

static void on_row(lv_event_t *e)
{
    settings_t *s = s_s;
    page_t p = (page_t)(intptr_t)lv_event_get_user_data(e);
    if (!s) {
        return;
    }
    show_page(s, p);
    if (p == PG_ABOUT || p == PG_UPDATE) {
        ui_ops_settings.update(ui_model());
    }
}

static void on_calibrate(lv_event_t *e)
{
    (void)e;
    ui_goto(UI_SCR_CALIBRATE, NULL);
}

#define ROW_H 48

static void list_row(lv_obj_t *page, page_t target, const char *icon, const char *text)
{
    lv_obj_t *b = ui_btn_create(page, UI_BTN_NEUTRAL, NULL, 0, 0, 228, ROW_H);
    if (target == PG_COUNT) { /* not a page: the calibration screen */
        lv_obj_add_event_cb(b, on_calibrate, LV_EVENT_CLICKED, NULL);
    } else {
        lv_obj_add_event_cb(b, on_row, LV_EVENT_CLICKED, (void *)(intptr_t)target);
    }
    lv_obj_t *ic = ui_label_create(b, &lv_font_montserrat_24, UI_COL_BLUE, icon);
    lv_obj_align(ic, LV_ALIGN_LEFT_MID, 12, 0);
    lv_obj_t *t = ui_label_create(b, &lv_font_montserrat_24, UI_COL_TEXT, text);
    lv_obj_align(t, LV_ALIGN_LEFT_MID, 48, 0);
    lv_obj_t *ch = ui_label_create(b, &lv_font_montserrat_24, UI_COL_DIM, LV_SYMBOL_RIGHT);
    lv_obj_align(ch, LV_ALIGN_RIGHT_MID, -10, 0);
}

static lv_obj_t *form_create(settings_t *s, lv_obj_t *scr)
{
    lv_obj_t *f = ui_box_create(scr, 0, BODY_Y, UI_W, BODY_H);
    lv_obj_set_scrollable(f, true);
    lv_obj_set_clickable(f, true);
    lv_obj_set_scroll_dir(f, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(f, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(f, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(f, 6, 0);
    lv_obj_set_style_pad_row(f, 2, 0);
    lv_obj_add_event_cb(f, form_clicked, LV_EVENT_CLICKED, s);
    return f;
}

static lv_obj_t *field_create(settings_t *s, lv_obj_t *form, const char *caption,
                              const char *placeholder, uint32_t max_len, bool password)
{
    lv_obj_t *cap = ui_label_create(form, &lv_font_montserrat_14, UI_COL_DIM, caption);
    lv_obj_set_style_pad_top(cap, 6, 0);
    lv_obj_t *row = ui_box_create(form, 0, 0, 228, FIELD_H);
    lv_obj_t *ta = lv_textarea_create(row);
    ui_style_textarea(ta);
    lv_obj_set_size(ta, password ? 178 : 228, FIELD_H);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_placeholder_text(ta, placeholder);
    lv_textarea_set_max_length(ta, max_len);
    lv_obj_set_scroll_on_focus(ta, false);
    lv_obj_set_scrollbar_mode(ta, LV_SCROLLBAR_MODE_OFF);   /* no scroll indicator under long text */
    lv_obj_add_event_cb(ta, field_clicked, LV_EVENT_CLICKED, s);
    if (password) {
        lv_textarea_set_password_mode(ta, true);
        lv_obj_t *eye = ui_btn_create(row, UI_BTN_NEUTRAL, NULL, 184, 0, 44, FIELD_H);
        ui_label_create(eye, &lv_font_montserrat_24, UI_COL_TEXT, LV_SYMBOL_EYE_CLOSE);
        lv_obj_center(lv_obj_get_child(eye, 0));
        lv_obj_add_event_cb(eye, eye_clicked, LV_EVENT_CLICKED, ta);
    }
    return ta;
}

static lv_obj_t *about_row(lv_obj_t *page, const char *caption, int32_t y)
{
    lv_obj_t *cap = ui_label_create(page, &lv_font_montserrat_14, UI_COL_DIM, caption);
    lv_obj_set_pos(cap, 12, y);
    lv_obj_t *val = ui_fit_create(page, 12, y + 18, 216, 30, UI_COL_TEXT);
    lv_obj_set_style_text_align(lv_obj_get_child(val, 0), LV_TEXT_ALIGN_LEFT, 0);
    return val;
}

static void tz_pressed(lv_event_t *e)
{
    kb_hide(lv_event_get_user_data(e));
}

static void tz_changed(lv_event_t *e)
{
    settings_t *s = lv_event_get_user_data(e);
    uint32_t idx = lv_dropdown_get_selected(s->tz_dd);
    if (idx == ZONE_CUSTOM && s->tz_prev != ZONE_CUSTOM && !lv_textarea_get_text(s->tz_custom)[0]) {
        lv_textarea_set_text(s->tz_custom, ZONE[s->tz_prev].posix);   /* start editing from it */
    }
    s->tz_prev = idx;
    field_set_hidden(s->tz_custom, idx != ZONE_CUSTOM);
}

static void on_ota_install(lv_event_t *e)
{
    settings_t *s = lv_event_get_user_data(e);
    const char *url = lv_textarea_get_text(s->ota_url);
    if (!url[0]) {
        ui_toast("Enter the firmware URL");
        return;
    }
    kb_hide(s);
    ui_act_ota_start(url);
}

/* Update page widgets from the model: version line, button enable, bar, status text. */
static void update_ota(settings_t *s, const ui_model_t *m)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "%s - %s", m->fw_version[0] ? m->fw_version : "?",
             m->fw_partition[0] ? m->fw_partition : "?");
    ui_fit_set(s->fw, buf, &lv_font_montserrat_24, false);

    bool busy = m->ota_state == UI_OTA_RUNNING || m->ota_state == UI_OTA_DONE;
    bool in_call = m->call != UI_CALL_IDLE;
    if (busy || in_call) {
        lv_obj_add_state(s->ota_btn, LV_STATE_DISABLED);
    } else {
        lv_obj_remove_state(s->ota_btn, LV_STATE_DISABLED);
    }
    lv_obj_set_hidden(s->ota_bar, !busy);
    lv_bar_set_value(s->ota_bar, m->ota_percent, LV_ANIM_OFF);

    const char *msg = m->ota_message;
    lv_color_t col = UI_COL_DIM;
    if (m->ota_state == UI_OTA_FAILED) {
        col = UI_COL_BAD;
    } else if (m->ota_state == UI_OTA_DONE) {
        col = UI_COL_OK;
    } else if (busy) {
        col = UI_COL_TEXT;
    } else if (in_call) {
        msg = "Not available during a call";
    }
    lv_label_set_text(s->ota_msg, msg);
    lv_obj_set_style_text_color(s->ota_msg, col, 0);
}

/* ---- BLF keys page ---------------------------------------------------------- */

static void blf_remove_async(void *arg);

static void blf_remove_clicked(lv_event_t *e)
{
    settings_t *s = s_s;
    intptr_t i = (intptr_t)lv_event_get_user_data(e);
    if (!s || i < 0 || i >= s->blf_n) {
        return;
    }
    memmove(&s->blf[i], &s->blf[i + 1], (size_t)(s->blf_n - i - 1) * sizeof(s->blf[0]));
    s->blf_n--;
    /* the rows (this button included) are rebuilt after this callback returns */
    lv_async_call(blf_remove_async, s);
}

static void blf_remove_async(void *arg)
{
    if (arg == s_s) {
        blf_rebuild(s_s);
    }
}

static void blf_rebuild(settings_t *s)
{
    lv_obj_clean(s->blf_rows);
    for (int i = 0; i < s->blf_n; i++) {
        lv_obj_t *row = ui_box_create(s->blf_rows, 0, 0, 228, FIELD_H);
        lv_obj_set_style_bg_color(row, UI_COL_SURFACE, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(row, 8, 0);
        lv_obj_t *txt = ui_box_create(row, 10, 0, 228 - 10 - 52, FIELD_H);
        lv_obj_set_flex_flow(txt, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(txt, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(txt, 8, 0);
        lv_obj_t *ext = ui_label_create(txt, &lv_font_montserrat_24, UI_COL_TEXT, s->blf[i].ext);
        lv_label_set_long_mode(ext, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_style_max_width(ext, 228 - 10 - 52, 0);
        lv_obj_t *lab = ui_label_create(txt, &lv_font_montserrat_14, UI_COL_DIM, s->blf[i].label);
        lv_label_set_long_mode(lab, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_flex_grow(lab, 1);
        lv_obj_t *del = ui_btn_create(row, UI_BTN_RED, NULL, 228 - 44, 0, 44, FIELD_H);
        ui_label_create(del, &lv_font_montserrat_24, UI_COL_TEXT, LV_SYMBOL_TRASH);
        lv_obj_center(lv_obj_get_child(del, 0));
        lv_obj_add_event_cb(del, blf_remove_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
    if (s->blf_n) {
        lv_label_set_text_fmt(s->blf_cap, "Keys (%d of %d)", s->blf_n, UI_BLF_MAX);
    } else {
        lv_label_set_text(s->blf_cap, "No keys: the home screen shows no panel");
    }
    if (s->blf_n >= UI_BLF_MAX) {
        lv_obj_add_state(s->blf_add, LV_STATE_DISABLED);
    } else {
        lv_obj_remove_state(s->blf_add, LV_STATE_DISABLED);
    }
}

/* Same rules as the device's `blf` setting (components/common/sipwire_blf.h). */
static bool blf_ext_ok(const char *t)
{
    if (!t[0] || strlen(t) >= UI_BLF_EXT_LEN) {
        return false;
    }
    for (; *t; t++) {
        char c = *t;
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || strchr("*#+._-", c))) {
            return false;
        }
    }
    return true;
}

static bool blf_label_ok(const char *t, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if ((unsigned char)t[i] < 0x20 || t[i] == ',' || t[i] == ':') {
            return false;
        }
    }
    return true;
}

static void blf_add_clicked(lv_event_t *e)
{
    settings_t *s = lv_event_get_user_data(e);
    const char *ext = lv_textarea_get_text(s->blf_ext);
    const char *lab = lv_textarea_get_text(s->blf_label);
    /* labels keep inner spaces but not leading / trailing ones */
    while (*lab == ' ') {
        lab++;
    }
    size_t ll = strlen(lab);
    while (ll > 0 && lab[ll - 1] == ' ') {
        ll--;
    }
    if (s->blf_n >= UI_BLF_MAX) {
        ui_toast("At most 8 keys");
        return;
    }
    if (!blf_ext_ok(ext)) {
        ui_toast("Extension: 0-9 a-z * # + . _ -");
        return;
    }
    if (!blf_label_ok(lab, ll) || ll >= UI_BLF_LABEL_LEN) {
        ui_toast("Label: up to 23 chars, no , or :");
        return;
    }
    for (int i = 0; i < s->blf_n; i++) {
        if (strcmp(s->blf[i].ext, ext) == 0) {
            ui_toast("Already in the list");
            return;
        }
    }
    ui_blf_t *b = &s->blf[s->blf_n++];
    memset(b, 0, sizeof(*b));
    snprintf(b->ext, sizeof(b->ext), "%s", ext);
    snprintf(b->label, sizeof(b->label), "%.*s", (int)ll, lab);
    b->state = UI_BLF_UNKNOWN;
    lv_textarea_set_text(s->blf_ext, "");
    lv_textarea_set_text(s->blf_label, "");
    kb_hide(s);
    blf_rebuild(s);
}

static void settings_toast_back(settings_t *s, const char *msg)
{
    ui_toast(msg);
    show_page(s, PG_LIST);
}

static void on_save(lv_event_t *e)
{
    settings_t *s = lv_event_get_user_data(e);
    if (s->cur == PG_WIFI) {
        const char *ssid = lv_textarea_get_text(s->ssid);
        if (!ssid[0]) {
            ui_toast("Enter the network name");
            return;
        }
        ui_act_save_wifi(ssid, lv_textarea_get_text(s->wpass));
        settings_toast_back(s, "Wi-Fi saved");
    } else if (s->cur == PG_ACCOUNT) {
        const char *server = lv_textarea_get_text(s->server);
        const char *user = lv_textarea_get_text(s->user);
        const char *ptxt = lv_textarea_get_text(s->port);
        if (!server[0] || !user[0]) {
            ui_toast("Server and user are required");
            return;
        }
        long port = ptxt[0] ? strtol(ptxt, NULL, 10) : DEFAULT_SIP_PORT;
        if (port < 1 || port > 65535) {
            ui_toast("Port must be 1-65535");
            return;
        }
        ui_act_save_account(server, (uint16_t)port, user, lv_textarea_get_text(s->apass),
                            lv_textarea_get_text(s->dname));
        settings_toast_back(s, "Account saved");
    } else if (s->cur == PG_BLF) {
        char list[UI_BLF_MAX * (UI_BLF_EXT_LEN + UI_BLF_LABEL_LEN + 1)];
        size_t n = 0;
        list[0] = '\0';
        for (int i = 0; i < s->blf_n; i++) {
            n += (size_t)snprintf(list + n, sizeof(list) - n, "%s%s%s%s", i ? "," : "", s->blf[i].ext,
                                  s->blf[i].label[0] ? ":" : "", s->blf[i].label);
        }
        const char *vm = lv_textarea_get_text(s->vm);
        if (vm[0] && !blf_ext_ok(vm)) {
            ui_toast("Voicemail: 0-9 a-z * # + . _ -");
            return;
        }
        ui_act_save_blf(list);
        ui_act_save_vm_number(vm);
        settings_toast_back(s, "BLF keys saved");
    } else if (s->cur == PG_TIME) {
        const char *ntp = lv_textarea_get_text(s->ntp);
        uint32_t idx = lv_dropdown_get_selected(s->tz_dd);
        const char *tz = idx == ZONE_CUSTOM ? lv_textarea_get_text(s->tz_custom) : ZONE[idx].posix;
        if (!ntp[0]) {
            ui_toast("Enter an NTP server");
            return;
        }
        if (!tz[0]) {
            ui_toast("Enter a TZ string");
            return;
        }
        ui_act_save_time(ntp, tz, lv_obj_has_state(s->h24, LV_STATE_CHECKED));
        settings_toast_back(s, "Time saved");
    }
}

/* ---- Sounds ----------------------------------------------------------------- */

static void tone_paint(settings_t *s)
{
    for (int i = 0; i < s->tone_n; i++) {
        lv_obj_t *r = s->tone_row[i];
        bool sel = i == s->tone_sel, play = i == s->tone_playing;
        lv_obj_set_style_border_width(r, sel ? 2 : 0, 0);
        lv_obj_set_style_border_color(r, UI_COL_BLUE, 0);
        lv_obj_t *ic = lv_obj_get_child(r, 1);
        lv_label_set_text(ic, play ? LV_SYMBOL_VOLUME_MAX : (sel ? LV_SYMBOL_OK : ""));
        lv_obj_set_style_text_color(ic, play ? UI_COL_OK : UI_COL_BLUE, 0);
    }
}

static void sounds_stop(settings_t *s)
{
    if (s->tone_playing >= 0) {
        s->tone_playing = -1;
        ui_act_ringtone_preview(-1);
        tone_paint(s);
    }
}

static void sounds_save(settings_t *s)
{
    ui_act_save_sounds((uint8_t)s->tone_sel, (uint8_t)lv_slider_get_value(s->vol[0]),
                       (uint8_t)lv_slider_get_value(s->vol[1]), (uint8_t)lv_slider_get_value(s->vol[2]));
}

static void on_tone(lv_event_t *e)
{
    settings_t *s = s_s;
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (!s || i < 0 || i >= s->tone_n) {
        return;
    }
    s->tone_sel = i;
    if (s->tone_playing == i) {
        s->tone_playing = -1;               /* tap the playing one again: stop */
        ui_act_ringtone_preview(-1);
    } else {
        s->tone_playing = i;                /* replaces any other preview */
        ui_act_ringtone_preview(i);
    }
    tone_paint(s);
    sounds_save(s);
}

static const char *const VOL_NAMES[SLIDER_ROWS] = {"Ring volume", "Call volume", "Speaker volume"};

static void vol_caption(settings_t *s, int i)
{
    lv_label_set_text_fmt(s->vol_cap[i], "%s  %d%%", VOL_NAMES[i], (int)lv_slider_get_value(s->vol[i]));
}

static void on_vol(lv_event_t *e)
{
    settings_t *s = s_s;
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (!s) {
        return;
    }
    vol_caption(s, i);
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) {
        sounds_save(s);
    } else if (i == 0) {
        ui_act_set_volume((uint8_t)lv_slider_get_value(s->vol[0]));   /* a running preview follows live */
    }
}

static void sounds_prefill(settings_t *s)
{
    const ui_model_t *m = ui_model();
    int n = m->ringtone_count > TONE_MAX ? TONE_MAX : m->ringtone_count;
    for (; s->tone_n < n; s->tone_n++) {
        int i = s->tone_n;
        const char *name = ui_act_ringtone_name(i);
        lv_obj_t *r = ui_btn_create(s->tone_box, UI_BTN_NEUTRAL, NULL, 0, 0, 228, 42);
        lv_obj_t *l = ui_label_create(r, &lv_font_montserrat_24, UI_COL_TEXT, name ? name : "?");
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 10, 0);
        lv_obj_t *ic = ui_label_create(r, &lv_font_montserrat_24, UI_COL_BLUE, "");
        lv_obj_align(ic, LV_ALIGN_RIGHT_MID, -10, 0);
        lv_obj_add_event_cb(r, on_tone, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        s->tone_row[i] = r;
    }
    s->tone_sel = m->ring_tone < s->tone_n ? m->ring_tone : 0;
    s->tone_playing = -1;
    tone_paint(s);
    const uint8_t v[SLIDER_ROWS] = {m->ring_volume, m->call_volume, m->speaker_volume};
    for (int i = 0; i < SLIDER_ROWS; i++) {
        lv_slider_set_value(s->vol[i], v[i], LV_ANIM_OFF);
        vol_caption(s, i);
    }
}

static lv_obj_t *slider_row(settings_t *s, lv_obj_t *page, int i)
{
    s->vol_cap[i] = ui_label_create(page, &lv_font_montserrat_14, UI_COL_DIM, "");
    lv_obj_set_style_pad_top(s->vol_cap[i], 8, 0);
    lv_obj_t *row = ui_box_create(page, 0, 0, 228, 40);
    lv_obj_t *sl = lv_slider_create(row);
    lv_obj_set_size(sl, 200, 12);
    lv_obj_align(sl, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_ext_click_area(sl, 14);
    lv_slider_set_range(sl, 0, 100);
    lv_obj_set_style_bg_color(sl, UI_COL_SURFACE2, 0);
    lv_obj_set_style_bg_opa(sl, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(sl, UI_COL_BLUE, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(sl, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sl, UI_COL_TEXT, LV_PART_KNOB);
    lv_obj_set_style_bg_opa(sl, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_pad_all(sl, 9, LV_PART_KNOB);
    lv_obj_add_event_cb(sl, on_vol, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)i);
    lv_obj_add_event_cb(sl, on_vol, LV_EVENT_RELEASED, (void *)(intptr_t)i);
    s->vol[i] = sl;
    return sl;
}

/* ---- Call features ---------------------------------------------------------- */

static void features_save(settings_t *s)
{
    ui_act_save_call_features(lv_obj_has_state(s->sw_cw, LV_STATE_CHECKED),
                              lv_obj_has_state(s->sw_aa, LV_STATE_CHECKED),
                              lv_obj_has_state(s->sw_dnd, LV_STATE_CHECKED));
}

static void on_feature(lv_event_t *e)
{
    (void)e;
    if (s_s) {
        features_save(s_s);
    }
}

static void set_switch(lv_obj_t *sw, bool on)
{
    if (on) {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    } else {
        lv_obj_remove_state(sw, LV_STATE_CHECKED);
    }
}

#define QUIET_STEP_MIN 30           /* dropdown granularity (console `cfg` takes any minute) */
#define QUIET_STEPS (1440 / QUIET_STEP_MIN)

static void quiet_save(settings_t *s)
{
    ui_act_save_quiet(lv_obj_has_state(s->sw_quiet, LV_STATE_CHECKED), s->q_start, s->q_end,
                      lv_obj_has_state(s->sw_qallow, LV_STATE_CHECKED));
}

static void on_quiet(lv_event_t *e)
{
    settings_t *s = s_s;
    if (!s) {
        return;
    }
    lv_obj_t *t = lv_event_get_target_obj(e);
    if (t == s->dd_qstart) {
        s->q_start = (uint16_t)(lv_dropdown_get_selected(t) * QUIET_STEP_MIN);
    } else if (t == s->dd_qend) {
        s->q_end = (uint16_t)(lv_dropdown_get_selected(t) * QUIET_STEP_MIN);
    }
    quiet_save(s);
}

/* "22:00\n22:30..." or "10:00 PM..." per the clock setting (LVGL heap, copied by the dropdown). */
static void quiet_options(lv_obj_t *dd, bool h24)
{
    char *buf = lv_malloc(QUIET_STEPS * 10);
    if (!buf) {
        return;
    }
    size_t n = 0;
    for (int i = 0; i < QUIET_STEPS; i++) {
        int h = i * QUIET_STEP_MIN / 60, m = i * QUIET_STEP_MIN % 60;
        if (h24) {
            n += (size_t)lv_snprintf(buf + n, QUIET_STEPS * 10 - n, "%s%02d:%02d", i ? "\n" : "", h, m);
        } else {
            n += (size_t)lv_snprintf(buf + n, QUIET_STEPS * 10 - n, "%s%d:%02d %s", i ? "\n" : "",
                                     h % 12 ? h % 12 : 12, m, h < 12 ? "AM" : "PM");
        }
    }
    lv_dropdown_set_options(dd, buf);
    lv_free(buf);
}

static void features_prefill(settings_t *s)
{
    const ui_model_t *m = ui_model();
    set_switch(s->sw_cw, m->call_waiting);
    set_switch(s->sw_aa, m->auto_answer);
    set_switch(s->sw_dnd, m->dnd);
    set_switch(s->sw_quiet, m->quiet_hours);
    set_switch(s->sw_qallow, m->quiet_allow_contacts);
    s->q_start = m->quiet_start < 1440 ? m->quiet_start : 0;
    s->q_end = m->quiet_end < 1440 ? m->quiet_end : 0;
    quiet_options(s->dd_qstart, m->clock_24h);
    quiet_options(s->dd_qend, m->clock_24h);
    lv_dropdown_set_selected(s->dd_qstart, s->q_start / QUIET_STEP_MIN);
    lv_dropdown_set_selected(s->dd_qend, s->q_end / QUIET_STEP_MIN);
    lv_obj_scroll_to_y(s->page[PG_FEATURES], 0, LV_ANIM_OFF);
}

/* "From" / "Until" + a time dropdown, one row. */
static lv_obj_t *quiet_time_row(lv_obj_t *page, const char *caption)
{
    lv_obj_t *row = ui_box_create(page, 0, 0, 228, FIELD_H + 4);
    lv_obj_t *t = ui_label_create(row, &lv_font_montserrat_24, UI_COL_TEXT, caption);
    lv_obj_align(t, LV_ALIGN_LEFT_MID, 4, 0);
    lv_obj_t *dd = lv_dropdown_create(row);
    lv_obj_set_size(dd, 150, FIELD_H);
    lv_obj_align(dd, LV_ALIGN_RIGHT_MID, -2, 0);
    ui_style_dropdown(dd);
    lv_obj_add_event_cb(dd, on_quiet, LV_EVENT_VALUE_CHANGED, NULL);
    return dd;
}

static lv_obj_t *feature_row(lv_obj_t *page, const char *title, const char *desc, lv_event_cb_t cb)
{
    lv_obj_t *row = ui_box_create(page, 0, 0, 228, 64);
    lv_obj_t *t = ui_label_create(row, &lv_font_montserrat_24, UI_COL_TEXT, title);
    lv_obj_set_pos(t, 4, 6);
    lv_obj_t *d = ui_label_create(row, &lv_font_montserrat_14, UI_COL_DIM, desc);
    lv_obj_set_pos(d, 4, 38);
    lv_obj_t *sw = lv_switch_create(row);
    lv_obj_set_size(sw, 52, 30);
    lv_obj_align(sw, LV_ALIGN_TOP_RIGHT, -2, 4);
    lv_obj_set_style_bg_color(sw, UI_COL_SURFACE2, 0);
    lv_obj_set_style_bg_color(sw, UI_COL_GREEN, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(sw, UI_COL_TEXT, LV_PART_KNOB);
    lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, NULL);
    return sw;
}

static void settings_leave(void)
{
    if (s_s) {
        sounds_stop(s_s);
    }
}

static lv_obj_t *settings_create(const char *arg)
{
    (void)arg;
    settings_t *s = lv_zalloc(sizeof(*s));
    if (!s) {
        return NULL;
    }
    lv_obj_t *scr = ui_screen_create();
    lv_obj_add_event_cb(scr, on_delete, LV_EVENT_DELETE, s);

    /* pages first so the header/status bar sit above scrolled content */
    lv_obj_t *list = form_create(s, scr);
    s->page[PG_LIST] = list;
    lv_obj_set_style_pad_row(list, 6, 0);
    list_row(list, PG_WIFI, LV_SYMBOL_WIFI, "Wi-Fi");
    list_row(list, PG_ACCOUNT, LV_SYMBOL_CALL, "SIP account");
    list_row(list, PG_BLF, LV_SYMBOL_EYE_OPEN, "BLF keys");
    list_row(list, PG_COUNT, LV_SYMBOL_EDIT, "Calibrate");
    list_row(list, PG_SOUNDS, LV_SYMBOL_VOLUME_MAX, "Sounds");
    list_row(list, PG_FEATURES, LV_SYMBOL_SHUFFLE, "Call features");
    list_row(list, PG_TIME, LV_SYMBOL_BELL, "Time");
    list_row(list, PG_UPDATE, LV_SYMBOL_DOWNLOAD, "Update");
    list_row(list, PG_ABOUT, LV_SYMBOL_LIST, "About");

    s->page[PG_WIFI] = form_create(s, scr);
    s->ssid = field_create(s, s->page[PG_WIFI], "Network name (SSID)", "SSID", 32, false);
    s->wpass = field_create(s, s->page[PG_WIFI], "Password", "Wi-Fi password", 63, true);

    s->page[PG_ACCOUNT] = form_create(s, scr);
    s->server = field_create(s, s->page[PG_ACCOUNT], "Server", "sip.example.com", 63, false);
    s->port = field_create(s, s->page[PG_ACCOUNT], "Port", "5060", 5, false);
    lv_textarea_set_accepted_chars(s->port, "0123456789");
    s->user = field_create(s, s->page[PG_ACCOUNT], "Username", "1001", 63, false);
    s->apass = field_create(s, s->page[PG_ACCOUNT], "Password", "SIP password", 63, true);
    s->dname = field_create(s, s->page[PG_ACCOUNT], "Display name", "Optional", 63, false);

    static const char DIAL_CHARS[] = "0123456789*#+._-abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
    lv_obj_t *bf = form_create(s, scr);
    s->page[PG_BLF] = bf;
    s->blf_cap = ui_label_create(bf, &lv_font_montserrat_14, UI_COL_DIM, "");
    lv_obj_set_style_pad_top(s->blf_cap, 4, 0);
    s->blf_rows = ui_box_create(bf, 0, 0, 228, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s->blf_rows, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s->blf_rows, 4, 0);
    s->blf_ext = field_create(s, bf, "Add: extension", "1002", UI_BLF_EXT_LEN - 1, false);
    lv_textarea_set_accepted_chars(s->blf_ext, DIAL_CHARS);
    s->blf_label = field_create(s, bf, "Label (optional)", "Bob", UI_BLF_LABEL_LEN - 1, false);
    s->blf_add = ui_btn_create(bf, UI_BTN_BLUE, LV_SYMBOL_PLUS " Add key", 0, 0, 228, FIELD_H);
    lv_obj_set_style_margin_top(s->blf_add, 8, 0);
    lv_obj_set_style_opa(s->blf_add, LV_OPA_40, LV_STATE_DISABLED);
    lv_obj_add_event_cb(s->blf_add, blf_add_clicked, LV_EVENT_CLICKED, s);
    s->vm = field_create(s, bf, "Voicemail number", "*97", 31, false);
    lv_textarea_set_accepted_chars(s->vm, DIAL_CHARS);

    lv_obj_t *tf = form_create(s, scr);
    s->page[PG_TIME] = tf;
    s->ntp = field_create(s, tf, "NTP server", "pool.ntp.org", 63, false);
    lv_obj_t *tzcap = ui_label_create(tf, &lv_font_montserrat_14, UI_COL_DIM, "Time zone");
    lv_obj_set_style_pad_top(tzcap, 6, 0);
    s->tz_dd = lv_dropdown_create(tf);
    lv_dropdown_set_options_static(s->tz_dd, ZONE_OPTIONS);
    lv_obj_set_size(s->tz_dd, 228, FIELD_H);
    ui_style_dropdown(s->tz_dd);
    lv_obj_add_event_cb(s->tz_dd, tz_pressed, LV_EVENT_PRESSED, s);
    lv_obj_add_event_cb(s->tz_dd, tz_changed, LV_EVENT_VALUE_CHANGED, s);
    s->tz_custom = field_create(s, tf, "POSIX TZ string", "EST5EDT,M3.2.0,M11.1.0", 63, false);
    lv_obj_t *hcap = ui_label_create(tf, &lv_font_montserrat_14, UI_COL_DIM, "Clock");
    lv_obj_set_style_pad_top(hcap, 6, 0);
    lv_obj_t *hrow = ui_box_create(tf, 0, 0, 228, FIELD_H);
    ui_label_create(hrow, &lv_font_montserrat_24, UI_COL_TEXT, "24-hour");
    lv_obj_align(lv_obj_get_child(hrow, 0), LV_ALIGN_LEFT_MID, 4, 0);
    s->h24 = lv_switch_create(hrow);
    lv_obj_set_size(s->h24, 64, 34);
    lv_obj_align(s->h24, LV_ALIGN_RIGHT_MID, -4, 0);
    lv_obj_set_style_bg_color(s->h24, UI_COL_SURFACE2, 0);
    lv_obj_set_style_bg_color(s->h24, UI_COL_GREEN, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(s->h24, UI_COL_TEXT, LV_PART_KNOB);
    lv_obj_add_event_cb(s->h24, tz_pressed, LV_EVENT_PRESSED, s);

    lv_obj_t *uf = form_create(s, scr);
    s->page[PG_UPDATE] = uf;
    lv_obj_t *fwcap = ui_label_create(uf, &lv_font_montserrat_14, UI_COL_DIM, "Firmware - running slot");
    lv_obj_set_style_pad_top(fwcap, 4, 0);
    s->fw = ui_fit_create(uf, 0, 0, 228, 30, UI_COL_TEXT);
    lv_obj_set_style_text_align(lv_obj_get_child(s->fw, 0), LV_TEXT_ALIGN_LEFT, 0);
    s->ota_url = field_create(s, uf, "Firmware URL", "http://host:8090/ota.bin", OTA_URL_MAX_CHARS, false);
    s->ota_btn = ui_btn_create(uf, UI_BTN_BLUE, "Check & install", 0, 0, 228, FIELD_H + 4);
    lv_obj_set_style_margin_top(s->ota_btn, 8, 0);
    lv_obj_set_style_opa(s->ota_btn, LV_OPA_40, LV_STATE_DISABLED);
    lv_obj_add_event_cb(s->ota_btn, on_ota_install, LV_EVENT_CLICKED, s);
    s->ota_bar = lv_bar_create(uf);
    lv_obj_set_size(s->ota_bar, 228, 14);
    lv_obj_set_style_margin_top(s->ota_bar, 6, 0);
    lv_obj_set_style_bg_color(s->ota_bar, UI_COL_SURFACE2, 0);
    lv_obj_set_style_bg_opa(s->ota_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s->ota_bar, 7, 0);
    lv_obj_set_style_bg_color(s->ota_bar, UI_COL_GREEN, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s->ota_bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s->ota_bar, 7, LV_PART_INDICATOR);
    lv_bar_set_range(s->ota_bar, 0, 100);
    s->ota_msg = ui_label_create(uf, &lv_font_montserrat_14, UI_COL_DIM, "");
    lv_obj_set_width(s->ota_msg, 228);
    lv_obj_set_style_pad_top(s->ota_msg, 4, 0);

    lv_obj_t *sf = form_create(s, scr);
    s->page[PG_SOUNDS] = sf;
    lv_obj_t *tcap = ui_label_create(sf, &lv_font_montserrat_14, UI_COL_DIM, "Ringtone (tap to preview)");
    lv_obj_set_style_pad_top(tcap, 4, 0);
    s->tone_box = ui_box_create(sf, 0, 0, 228, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s->tone_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s->tone_box, 4, 0);
    s->tone_playing = -1;
    for (int i = 0; i < SLIDER_ROWS; i++) {
        slider_row(s, sf, i);
    }

    lv_obj_t *ff = form_create(s, scr);
    s->page[PG_FEATURES] = ff;
    s->sw_cw = feature_row(ff, "Call waiting", "Beep for a second call", on_feature);
    s->sw_aa = feature_row(ff, "Auto-answer", "Pages on the loudspeaker", on_feature);
    s->sw_dnd = feature_row(ff, "Don't disturb", "Refuse calls silently", on_feature);
    s->sw_quiet = feature_row(ff, "Quiet hours", "Like DND, every day", on_quiet);
    s->dd_qstart = quiet_time_row(ff, "From");
    s->dd_qend = quiet_time_row(ff, "Until");
    s->sw_qallow = feature_row(ff, "Contacts ring", "Even in quiet hours", on_quiet);

    lv_obj_t *about = ui_box_create(scr, 0, BODY_Y, UI_W, BODY_H);
    s->page[PG_ABOUT] = about;
    s->about_ip = about_row(about, "IP address", 2);
    s->about_wifi = about_row(about, "Wi-Fi", 50);
    s->about_acc = about_row(about, "Account", 98);
    s->about_reg = about_row(about, "Registration", 146);
    char cap[40];
    snprintf(cap, sizeof(cap), "Firmware (LVGL %d.%d.%d)", LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR,
             LVGL_VERSION_PATCH);
    s->about_fw = about_row(about, cap, 194);

    /* header + status bar on top of the pages */
    lv_obj_t *back = ui_header_create(scr, TITLES[PG_LIST], UI_BAR_H, &s->title);
    lv_obj_add_event_cb(back, on_back, LV_EVENT_CLICKED, s);
    lv_obj_t *hdr = lv_obj_get_parent(back);
    s->save = ui_btn_create(hdr, UI_BTN_GREEN, "Save", UI_W - 76, 2, 72, 44);
    lv_obj_add_event_cb(s->save, on_save, LV_EVENT_CLICKED, s);
    ui_statusbar_create(scr);

    /* keyboard last: it overlays the lower part of the form pages */
    s->kb = lv_keyboard_create(scr);
    lv_obj_set_size(s->kb, UI_W, KB_H);
    lv_obj_align(s->kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(s->kb, UI_COL_BG, 0);
    lv_obj_set_style_bg_opa(s->kb, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s->kb, 0, 0);
    lv_obj_set_style_radius(s->kb, 0, 0);
    lv_obj_set_style_pad_all(s->kb, 4, 0);
    lv_obj_set_style_pad_gap(s->kb, 4, 0);
    lv_obj_set_style_bg_color(s->kb, UI_COL_SURFACE, LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(s->kb, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_text_color(s->kb, UI_COL_TEXT, LV_PART_ITEMS);
    lv_obj_set_style_text_font(s->kb, &lv_font_montserrat_14, LV_PART_ITEMS);
    lv_obj_set_style_radius(s->kb, 6, LV_PART_ITEMS);
    lv_obj_set_style_border_width(s->kb, 0, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(s->kb, 0, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(s->kb, UI_COL_BLUE, LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(s->kb, UI_COL_SURFACE2, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(s->kb, UI_COL_TEXT, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_keyboard_set_map(s->kb, LV_KEYBOARD_MODE_USER_1, DIAL_KB_MAP, DIAL_KB_CTRL);
    lv_obj_add_event_cb(s->kb, kb_event, LV_EVENT_READY, s);
    lv_obj_add_event_cb(s->kb, kb_event, LV_EVENT_CANCEL, s);

    s_s = s;
    show_page(s, PG_LIST);
    return scr;
}

static void settings_update(const ui_model_t *m)
{
    settings_t *s = s_s;
    if (!s) {
        return;
    }
    if (s->cur == PG_UPDATE) {
        update_ota(s, m);
        return;
    }
    if (s->cur != PG_ABOUT) {
        return;
    }
    ui_fit_set(s->about_ip, m->ip[0] ? m->ip : "-", &lv_font_montserrat_24, false);

    char buf[64];
    if (m->net == UI_NET_CONNECTED) {
        snprintf(buf, sizeof(buf), "%d dBm", m->rssi);
    } else if (m->net == UI_NET_CONNECTING) {
        snprintf(buf, sizeof(buf), "Connecting...");
    } else {
        snprintf(buf, sizeof(buf), "Off");
    }
    ui_fit_set(s->about_wifi, buf, &lv_font_montserrat_24, false);
    ui_fit_set(s->about_acc, m->account[0] ? m->account : "Not configured",
               m->account[0] ? &lv_font_montserrat_14 : &lv_font_montserrat_24, false);

    const char *reg;
    if (m->net == UI_NET_OFF) {
        reg = "Offline";
    } else if (m->reg == UI_REG_REGISTERED) {
        reg = "Ready";
    } else if (m->reg == UI_REG_REGISTERING) {
        reg = "Registering...";
    } else if (m->reg == UI_REG_FAILED) {
        reg = "Registration failed";
    } else {
        reg = "Offline";
    }
    ui_fit_set(s->about_reg, reg, &lv_font_montserrat_24, false);

    snprintf(buf, sizeof(buf), "%s - %s", m->fw_version[0] ? m->fw_version : "?",
             m->fw_partition[0] ? m->fw_partition : "?");
    ui_fit_set(s->about_fw, buf, &lv_font_montserrat_24, false);
}

const ui_screen_ops_t ui_ops_settings = {settings_create, settings_update, NULL, NULL, settings_leave};
