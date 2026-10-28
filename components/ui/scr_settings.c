/* Settings: list -> Wi-Fi / SIP account forms (on-screen keyboard) / About /
 * touch calibration screen. The forms are prefilled from the model when opened
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

typedef enum { PG_LIST, PG_WIFI, PG_ACCOUNT, PG_ABOUT, PG_COUNT } page_t;

typedef struct {
    lv_obj_t *title;
    lv_obj_t *save;
    lv_obj_t *page[PG_COUNT];
    lv_obj_t *kb;
    page_t cur;

    lv_obj_t *ssid, *wpass;
    lv_obj_t *server, *port, *user, *apass, *dname;

    lv_obj_t *about_ip, *about_acc, *about_reg, *about_wifi;
} settings_t;

static settings_t *s_s;

static const char *const TITLES[PG_COUNT] = {"Settings", "Wi-Fi", "Account", "About"};

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
    return p == PG_WIFI || p == PG_ACCOUNT;
}

static void kb_hide(settings_t *s)
{
    lv_keyboard_set_textarea(s->kb, NULL);
    lv_obj_set_hidden(s->kb, true);
    for (page_t p = PG_WIFI; p <= PG_ACCOUNT; p++) {
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
    lv_keyboard_set_mode(s->kb, ta == s->port ? LV_KEYBOARD_MODE_NUMBER
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

static void prefill(settings_t *s, page_t p)
{
    const ui_model_t *m = ui_model();
    if (p == PG_WIFI) {
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

static void show_page(settings_t *s, page_t p)
{
    kb_hide(s);
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
        lv_obj_set_hidden(s->save, false);
        lv_obj_scroll_to_y(s->page[p], 0, LV_ANIM_OFF);
    } else {
        lv_obj_set_hidden(s->save, true);
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
    if (p == PG_ABOUT) {
        ui_ops_settings.update(ui_model());
    }
}

static void on_calibrate(lv_event_t *e)
{
    (void)e;
    ui_goto(UI_SCR_CALIBRATE, NULL);
}

#define ROW_H 54
#define ROW_PITCH 60

static void list_row(lv_obj_t *page, int row, page_t target, const char *icon, const char *text)
{
    lv_obj_t *b = ui_btn_create(page, UI_BTN_NEUTRAL, NULL, 6, 6 + row * ROW_PITCH, 228, ROW_H);
    if (target == PG_COUNT) { /* not a page: the calibration screen */
        lv_obj_add_event_cb(b, on_calibrate, LV_EVENT_CLICKED, NULL);
    } else {
        lv_obj_add_event_cb(b, on_row, LV_EVENT_CLICKED, (void *)(intptr_t)target);
    }
    lv_obj_t *ic = ui_label_create(b, &lv_font_montserrat_24, UI_COL_BLUE, icon);
    lv_obj_align(ic, LV_ALIGN_LEFT_MID, 12, 0);
    /* 24 pt, or 14 pt when the text would not fit between icon and chevron */
    bool small = strlen(text) > 11;
    lv_obj_t *t = ui_label_create(b, small ? &lv_font_montserrat_14 : &lv_font_montserrat_24,
                                  UI_COL_TEXT, text);
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
    s->page[PG_LIST] = ui_box_create(scr, 0, BODY_Y, UI_W, BODY_H);
    list_row(s->page[PG_LIST], 0, PG_WIFI, LV_SYMBOL_WIFI, "Wi-Fi");
    list_row(s->page[PG_LIST], 1, PG_ACCOUNT, LV_SYMBOL_CALL, "SIP account");
    list_row(s->page[PG_LIST], 2, PG_COUNT, LV_SYMBOL_EDIT, "Touch calibration");
    list_row(s->page[PG_LIST], 3, PG_ABOUT, LV_SYMBOL_LIST, "About");

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

    lv_obj_t *about = ui_box_create(scr, 0, BODY_Y, UI_W, BODY_H);
    s->page[PG_ABOUT] = about;
    s->about_ip = about_row(about, "IP address", 6);
    s->about_wifi = about_row(about, "Wi-Fi", 56);
    s->about_acc = about_row(about, "Account", 106);
    s->about_reg = about_row(about, "Registration", 156);
    char ver[40];
    snprintf(ver, sizeof(ver), "sipwire  -  LVGL %d.%d.%d", LVGL_VERSION_MAJOR,
             LVGL_VERSION_MINOR, LVGL_VERSION_PATCH);
    lv_obj_t *vl = ui_label_create(about, &lv_font_montserrat_14, UI_COL_DIM, ver);
    lv_obj_align(vl, LV_ALIGN_BOTTOM_MID, 0, -8);

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
    lv_obj_add_event_cb(s->kb, kb_event, LV_EVENT_READY, s);
    lv_obj_add_event_cb(s->kb, kb_event, LV_EVENT_CANCEL, s);

    s_s = s;
    show_page(s, PG_LIST);
    return scr;
}

static void settings_update(const ui_model_t *m)
{
    settings_t *s = s_s;
    if (!s || s->cur != PG_ABOUT) {
        return;
    }
    ui_fit_set(s->about_ip, m->ip[0] ? m->ip : "-", &lv_font_montserrat_24, false);

    char buf[40];
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
}

const ui_screen_ops_t ui_ops_settings = {settings_create, settings_update, NULL, NULL, NULL};
