/* Status bar shown at the top of every screen: Wi-Fi bars, SIP registration
 * dot, clock, battery. While voicemail is waiting an amber envelope (with the
 * number of new messages) takes the place of the "SIP" caption. Do not disturb: a dim bell left of
 * the battery (tap = on) or a red "DND" badge (tap = off); inside quiet hours (DND off) a blue "Zz"
 * badge in its place (tap = DND on); with always-forward set an amber "FWD" badge takes the
 * place of both (calls are forwarded then, not refused; tap still toggles DND). Each screen owns its own bar; only the newest one is
 * kept up to date. */

#include "ui_priv.h"

#include <stdio.h>
#include <string.h>

#define WIFI_BARS 4

typedef struct {
    lv_obj_t *bar;
    lv_obj_t *wifi_bars[WIFI_BARS];
    lv_obj_t *wifi_off;
    lv_obj_t *reg_dot;
    lv_obj_t *reg_lbl;
    lv_obj_t *mwi;
    lv_obj_t *time;
    lv_obj_t *batt;
    lv_obj_t *dnd;      /* tap target: bell (off) / "DND" badge (on) */
    lv_obj_t *dnd_lbl;
    /* last applied values, to avoid needless redraws */
    bool valid;
    int lit;
    ui_net_state_t net;
    ui_reg_state_t reg;
    int8_t battery_pct;
    bool charging;
    char time_hhmm[UI_TIME_STR_LEN];
    bool mwi_waiting;
    uint16_t mwi_new;
    bool dnd_on;
    bool quiet_on;
    bool fwd_on;
} statusbar_t;

static statusbar_t *s_sb;

static void on_dnd(lv_event_t *e)
{
    (void)e;
    ui_act_set_dnd(!ui_model()->dnd);
}

static void sb_deleted(lv_event_t *e)
{
    statusbar_t *sb = lv_event_get_user_data(e);
    if (s_sb == sb) {
        s_sb = NULL;
    }
    lv_free(sb);
}

lv_obj_t *ui_statusbar_create(lv_obj_t *screen)
{
    statusbar_t *sb = lv_zalloc(sizeof(*sb));
    if (!sb) {
        return NULL;
    }
    lv_obj_t *bar = ui_box_create(screen, 0, 0, UI_W, UI_BAR_H);
    lv_obj_set_style_bg_color(bar, UI_COL_BAR, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(bar, sb_deleted, LV_EVENT_DELETE, sb);
    sb->bar = bar;

    /* Wi-Fi strength: four bars of rising height. */
    for (int i = 0; i < WIFI_BARS; i++) {
        int32_t h = 4 + i * 3;
        lv_obj_t *b = ui_box_create(bar, 8 + i * 5, 17 - h, 3, h);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(b, 1, 0);
        sb->wifi_bars[i] = b;
    }
    sb->wifi_off = ui_label_create(bar, &lv_font_montserrat_14, UI_COL_BAD, LV_SYMBOL_CLOSE);
    lv_obj_set_pos(sb->wifi_off, 30, 3);

    /* SIP registration. */
    sb->reg_dot = ui_box_create(bar, 54, 7, 8, 8);
    lv_obj_set_style_radius(sb->reg_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(sb->reg_dot, LV_OPA_COVER, 0);
    sb->reg_lbl = ui_label_create(bar, &lv_font_montserrat_14, UI_COL_DIM, "SIP");
    lv_obj_set_pos(sb->reg_lbl, 67, 3);
    sb->mwi = ui_label_create(bar, &lv_font_montserrat_14, UI_COL_WARN, "");
    lv_obj_set_pos(sb->mwi, 67, 3);
    lv_obj_set_hidden(sb->mwi, true);

    sb->time = ui_label_create(bar, &lv_font_montserrat_14, UI_COL_TEXT, "");
    lv_obj_set_pos(sb->time, 96, 3);

    sb->batt = ui_label_create(bar, &lv_font_montserrat_14, UI_COL_TEXT, "");
    lv_obj_align(sb->batt, LV_ALIGN_TOP_RIGHT, -6, 3);

    /* do not disturb toggle: small, so the click area reaches past the visible badge */
    sb->dnd = lv_obj_create(bar);
    lv_obj_remove_style_all(sb->dnd);
    lv_obj_set_size(sb->dnd, 18, 18);
    lv_obj_set_style_radius(sb->dnd, 4, 0);
    lv_obj_set_scrollable(sb->dnd, false);
    lv_obj_set_clickable(sb->dnd, true);
    lv_obj_set_ext_click_area(sb->dnd, 10);
    lv_obj_add_event_cb(sb->dnd, on_dnd, LV_EVENT_CLICKED, NULL);
    sb->dnd_lbl = ui_label_create(sb->dnd, &lv_font_montserrat_14, UI_COL_DIM, LV_SYMBOL_BELL);
    lv_obj_center(sb->dnd_lbl);

    s_sb = sb;
    return bar;
}

static int wifi_level(const ui_model_t *m)
{
    switch (m->net) {
    case UI_NET_CONNECTED:
        if (m->rssi >= -55) {
            return 4;
        }
        if (m->rssi >= -65) {
            return 3;
        }
        if (m->rssi >= -75) {
            return 2;
        }
        return 1;
    case UI_NET_CONNECTING:
        return 2;
    default:
        return 0;
    }
}

static const char *battery_symbol(int pct)
{
    if (pct > 85) {
        return LV_SYMBOL_BATTERY_FULL;
    }
    if (pct > 60) {
        return LV_SYMBOL_BATTERY_3;
    }
    if (pct > 35) {
        return LV_SYMBOL_BATTERY_2;
    }
    if (pct > 10) {
        return LV_SYMBOL_BATTERY_1;
    }
    return LV_SYMBOL_BATTERY_EMPTY;
}

void ui_statusbar_update(const ui_model_t *m)
{
    statusbar_t *sb = s_sb;
    if (!sb) {
        return;
    }

    int lit = wifi_level(m);
    if (!sb->valid || lit != sb->lit || m->net != sb->net) {
        lv_color_t on = (m->net == UI_NET_CONNECTING)
                            ? UI_COL_WARN
                            : (lit <= 1 ? UI_COL_WARN : UI_COL_TEXT);
        for (int i = 0; i < WIFI_BARS; i++) {
            lv_obj_set_style_bg_color(sb->wifi_bars[i], i < lit ? on : UI_COL_SURFACE2, 0);
        }
        if (m->net == UI_NET_OFF) {
            lv_obj_set_hidden(sb->wifi_off, false);
        } else {
            lv_obj_set_hidden(sb->wifi_off, true);
        }
        sb->lit = lit;
        sb->net = m->net;
    }

    if (!sb->valid || m->reg != sb->reg) {
        lv_color_t c;
        switch (m->reg) {
        case UI_REG_REGISTERED:  c = UI_COL_OK; break;
        case UI_REG_REGISTERING: c = UI_COL_WARN; break;
        case UI_REG_FAILED:      c = UI_COL_BAD; break;
        default:                 c = UI_COL_SURFACE2; break;
        }
        lv_obj_set_style_bg_color(sb->reg_dot, c, 0);
        sb->reg = m->reg;
    }

    bool fwd = m->fwd_always[0] != '\0';
    bool quiet = m->quiet_active && !m->dnd && !fwd;
    bool dnd_changed = !sb->valid || m->dnd != sb->dnd_on || quiet != sb->quiet_on || fwd != sb->fwd_on;
    bool badge = fwd || m->dnd || quiet;    /* FWD, DND or Zz badge: needs the room on the left too */
    bool mwi_changed = !sb->valid || m->mwi_waiting != sb->mwi_waiting || m->mwi_new != sb->mwi_new;
    if (mwi_changed || dnd_changed) {
        if (m->mwi_waiting) {
            if (m->mwi_new > 0) {
                lv_label_set_text_fmt(sb->mwi, LV_SYMBOL_ENVELOPE "%u", m->mwi_new > 99 ? 99u : (unsigned)m->mwi_new);
            } else {
                lv_label_set_text(sb->mwi, LV_SYMBOL_ENVELOPE);
            }
        }
        /* with DND on the badge needs the room: caption and envelope make way */
        lv_obj_set_hidden(sb->mwi, !m->mwi_waiting || badge);
        lv_obj_set_hidden(sb->reg_lbl, m->mwi_waiting || badge);
        sb->mwi_waiting = m->mwi_waiting;
        sb->mwi_new = m->mwi_new;
    }

    bool time_changed = !sb->valid || strncmp(m->time_hhmm, sb->time_hhmm, sizeof(sb->time_hhmm)) != 0;
    if (time_changed) {
        lv_label_set_text(sb->time, m->time_hhmm);
        snprintf(sb->time_hhmm, sizeof(sb->time_hhmm), "%s", m->time_hhmm);
    }

    bool batt_changed = !sb->valid || m->battery_pct != sb->battery_pct || m->charging != sb->charging;
    if (batt_changed) {
        if (m->battery_pct < 0) {
            lv_obj_set_hidden(sb->batt, true);
        } else {
            int pct = m->battery_pct > 100 ? 100 : m->battery_pct;
            lv_obj_set_hidden(sb->batt, false);
            lv_label_set_text_fmt(sb->batt, "%s%s %d%%", m->charging ? LV_SYMBOL_CHARGE " " : "",
                                  battery_symbol(pct), pct);
            lv_obj_set_style_text_color(
                sb->batt, (pct <= 15 && !m->charging) ? UI_COL_BAD : UI_COL_TEXT, 0);
        }
        sb->battery_pct = m->battery_pct;
        sb->charging = m->charging;
    }

    if (dnd_changed) {
        lv_label_set_text(sb->dnd_lbl, fwd ? "FWD" : m->dnd ? "DND" : quiet ? "Zz" : LV_SYMBOL_BELL);
        lv_obj_set_style_text_color(sb->dnd_lbl, fwd ? UI_COL_BG : badge ? UI_COL_TEXT : lv_color_hex(0x7D8996), 0);
        lv_obj_set_style_bg_color(sb->dnd, fwd ? UI_COL_WARN : m->dnd ? UI_COL_RED : UI_COL_BLUE, 0);
        lv_obj_set_style_bg_opa(sb->dnd, badge ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_text_letter_space(sb->dnd_lbl, fwd ? -1 : 0, 0);   /* "FWD" in 32 px */
        lv_obj_set_width(sb->dnd, fwd ? 32 : badge ? 30 : 18);
        lv_obj_center(sb->dnd_lbl);
        sb->dnd_on = m->dnd;
        sb->quiet_on = quiet;
        sb->fwd_on = fwd;
    }

    /* Right to left: battery at the edge, the DND bell / badge left of it, the clock centred
     * but kept clear of both (and of the SIP caption / envelope on the left). */
    if (dnd_changed || batt_changed || time_changed || mwi_changed) {
        int32_t right = UI_W - 6;
        if (!lv_obj_is_hidden(sb->batt)) {
            lv_obj_update_layout(sb->batt);
            right -= lv_obj_get_width(sb->batt) + 4;
        }
        lv_obj_update_layout(sb->dnd);
        int32_t dnd_x = right - lv_obj_get_width(sb->dnd);
        lv_obj_set_pos(sb->dnd, dnd_x, 2);

        lv_obj_update_layout(sb->time);
        int32_t tw = lv_obj_get_width(sb->time);
        int32_t left = badge ? 66 : (m->mwi_waiting ? (m->mwi_new >= 10 ? 104 : 96) : 96);
        int32_t x = UI_W / 2 - tw / 2 + (strchr(m->time_hhmm, ' ') ? 8 : 0);
        if (x + tw > dnd_x - 6) {
            x = dnd_x - 6 - tw;
        }
        /* A wide right side (battery % + charging + bell) pushes a 12 h clock into the SIP
         * caption / envelope: they make way, as for the DND badge (the dot stays). */
        bool squeezed = !badge && left + tw > dnd_x - 4;
        if (squeezed) {
            left = 66;
        }
        lv_obj_set_hidden(sb->mwi, !m->mwi_waiting || badge || squeezed);
        lv_obj_set_hidden(sb->reg_lbl, m->mwi_waiting || badge || squeezed);
        if (x < left) {
            x = left;
            if (x + tw > dnd_x - 4) { /* still too tight: drop the badge's padding gap, never overlap */
                x = dnd_x - 4 - tw;
            }
        }
        lv_obj_set_pos(sb->time, x, 3);
    }
    sb->valid = true;
}
