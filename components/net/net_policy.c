/* Pure Wi-Fi link policy (see net_policy.h). No IDF dependencies. */

#include "net_policy.h"

#include <string.h>

/* RSSI this much above a roam trigger ends a weak episode: the next one scans without the
 * backoff of the previous one. */
#define ROAM_RECOVER_DB 5

void net_roam_reset(net_roam_t *r, const net_roam_cfg_t *c, uint64_t now_ms)
{
    r->next_scan_ms = now_ms + c->interval_ms;
    r->backoff = 0;
}

bool net_roam_scan_due(net_roam_t *r, const net_roam_cfg_t *c, bool in_call, int rssi, uint64_t now_ms)
{
    int trigger = in_call ? c->call_dbm : c->idle_dbm;
    if (rssi == NET_RSSI_NONE) {
        return false;
    }
    if (rssi >= trigger + ROAM_RECOVER_DB) {
        r->backoff = 0;
        return false;
    }
    return rssi < trigger && now_ms >= r->next_scan_ms;
}

void net_roam_scan_init(net_roam_scan_t *s)
{
    memset(s, 0, sizeof(*s));
    s->rssi = NET_RSSI_NONE;
    s->cur_rssi = NET_RSSI_NONE;
}

void net_roam_scan_add(net_roam_scan_t *s, const uint8_t cur_bssid[6], const uint8_t bssid[6], int rssi,
                       uint8_t channel)
{
    s->seen++;
    if (cur_bssid && memcmp(cur_bssid, bssid, 6) == 0) {
        if (rssi > s->cur_rssi) {
            s->cur_rssi = rssi;
        }
        return;
    }
    if (rssi > s->rssi) {
        s->rssi = rssi;
        s->channel = channel;
        memcpy(s->bssid, bssid, 6);
    }
}

bool net_roam_evaluate(net_roam_t *r, const net_roam_cfg_t *c, const net_roam_scan_t *s, int cur_rssi,
                       uint64_t now_ms)
{
    int cur = cur_rssi > s->cur_rssi ? cur_rssi : s->cur_rssi;
    bool better = s->rssi != NET_RSSI_NONE && (cur == NET_RSSI_NONE || s->rssi >= cur + c->delta_db);
    if (better) {
        r->backoff = 0;
    } else if (r->backoff < c->max_backoff) {
        r->backoff++;
    }
    r->next_scan_ms = now_ms + ((uint64_t)c->interval_ms << r->backoff);
    return better;
}

void net_warn_reset(net_warn_t *w)
{
    memset(w, 0, sizeof(*w));
}

net_warn_action_t net_warn_update(net_warn_t *w, const net_warn_cfg_t *c, bool armed, int rssi,
                                  uint64_t now_ms)
{
    if (!armed) {
        net_warn_reset(w);
        return NET_WARN_NONE;
    }
    bool below = rssi == NET_RSSI_NONE || rssi < c->warn_dbm;
    if (!w->weak) {
        w->below = below ? (uint8_t)(w->below < UINT8_MAX ? w->below + 1 : w->below) : 0;
        uint8_t need = c->enter_samples ? c->enter_samples : 1;
        if (w->below < need) {
            return NET_WARN_NONE;
        }
        w->weak = true;
        w->next_ms = now_ms + c->repeat_ms;
        return NET_WARN_ENTER;
    }
    if (rssi != NET_RSSI_NONE && rssi >= c->warn_dbm + c->hyst_db) {
        net_warn_reset(w);
        return NET_WARN_CLEAR;
    }
    if (now_ms >= w->next_ms) {
        w->next_ms = now_ms + c->repeat_ms;
        return NET_WARN_REPEAT;
    }
    return NET_WARN_NONE;
}
