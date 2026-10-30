#pragma once

/* Pure Wi-Fi link policy used by net.c: when to look for a stronger access point, whether a
 * scan found one worth roaming to, and the weak-signal warning during calls. No IDF headers,
 * host-tested (test_host/net). Times are milliseconds on any monotonic clock. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NET_RSSI_NONE (-128)        /* no sample: not associated / AP not in the scan */

/* ---- roaming between APs of one network (same SSID) ---- */

typedef struct {
    int idle_dbm;                   /* scan for a stronger AP below this while no call exists */
    int call_dbm;                   /* the same during a call (lower: a roam gaps the audio) */
    int delta_db;                   /* a candidate must be at least this much stronger */
    uint32_t interval_ms;           /* between scans while weak */
    uint8_t max_backoff;            /* a scan without a better AP doubles the interval, up to
                                     * interval << max_backoff (single-AP homes) */
} net_roam_cfg_t;

typedef struct {
    uint64_t next_scan_ms;
    uint8_t backoff;
} net_roam_t;

/** (Re)connected: no scan before now + interval (the connect already picked the strongest AP). */
void net_roam_reset(net_roam_t *r, const net_roam_cfg_t *c, uint64_t now_ms);

/** One RSSI sample of the current AP. True: start a roam scan now. */
bool net_roam_scan_due(net_roam_t *r, const net_roam_cfg_t *c, bool in_call, int rssi, uint64_t now_ms);

/** Result of one scan, folded record by record (net_roam_scan_add). */
typedef struct {
    uint8_t bssid[6];               /* best AP other than the current one */
    uint8_t channel;
    int rssi;                       /* NET_RSSI_NONE: none seen */
    int cur_rssi;                   /* the current AP as seen in the scan, NET_RSSI_NONE if not */
    unsigned seen;                  /* records of our SSID */
} net_roam_scan_t;

void net_roam_scan_init(net_roam_scan_t *s);
void net_roam_scan_add(net_roam_scan_t *s, const uint8_t cur_bssid[6], const uint8_t bssid[6], int rssi,
                       uint8_t channel);

/** The scan finished. cur_rssi: the driver's RSSI of the current AP (NET_RSSI_NONE if unknown);
 *  the stronger of it and the scan's own reading counts. True: roam to s->bssid. Sets the next
 *  scan time (backing off when nothing better was found). */
bool net_roam_evaluate(net_roam_t *r, const net_roam_cfg_t *c, const net_roam_scan_t *s, int cur_rssi,
                       uint64_t now_ms);

/* ---- weak-signal (out-of-range) warning during calls ---- */

typedef struct {
    int warn_dbm;                   /* weak below this */
    int hyst_db;                    /* normal again at warn_dbm + hyst_db or better */
    uint32_t repeat_ms;             /* warning period while weak */
    uint8_t enter_samples;          /* consecutive samples below warn_dbm before the first (>= 1) */
} net_warn_cfg_t;

typedef struct {
    bool weak;
    uint8_t below;                  /* consecutive samples below warn_dbm */
    uint64_t next_ms;               /* next repeat while weak */
} net_warn_t;

typedef enum {
    NET_WARN_NONE,
    NET_WARN_ENTER,                 /* became weak: warn */
    NET_WARN_REPEAT,                /* still weak, period elapsed: warn again */
    NET_WARN_CLEAR,                 /* back to normal */
} net_warn_action_t;

void net_warn_reset(net_warn_t *w);

/** One RSSI sample (NET_RSSI_NONE while not associated counts as weak). armed: a call exists;
 *  not armed resets the state silently (NET_WARN_NONE). */
net_warn_action_t net_warn_update(net_warn_t *w, const net_warn_cfg_t *c, bool armed, int rssi,
                                  uint64_t now_ms);
