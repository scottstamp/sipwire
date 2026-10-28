/* Status bar shown at the top of every screen: Wi-Fi bars, SIP registration
 * dot, clock, battery. Each screen owns its own bar; only the newest one is
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
    lv_obj_t *time;
    lv_obj_t *batt;
    /* last applied values, to avoid needless redraws */
    bool valid;
    int lit;
    ui_net_state_t net;
    ui_reg_state_t reg;
    int8_t battery_pct;
    bool charging;
    char time_hhmm[6];
} statusbar_t;

static statusbar_t *s_sb;

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

    sb->time = ui_label_create(bar, &lv_font_montserrat_14, UI_COL_TEXT, "");
    lv_obj_align(sb->time, LV_ALIGN_TOP_MID, 0, 3);

    sb->batt = ui_label_create(bar, &lv_font_montserrat_14, UI_COL_TEXT, "");
    lv_obj_align(sb->batt, LV_ALIGN_TOP_RIGHT, -6, 3);

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

    if (!sb->valid || strncmp(m->time_hhmm, sb->time_hhmm, sizeof(sb->time_hhmm)) != 0) {
        lv_label_set_text(sb->time, m->time_hhmm);
        snprintf(sb->time_hhmm, sizeof(sb->time_hhmm), "%s", m->time_hhmm);
    }

    if (!sb->valid || m->battery_pct != sb->battery_pct || m->charging != sb->charging) {
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
    sb->valid = true;
}
