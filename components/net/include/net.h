#pragma once

#include <stdbool.h>
#include "esp_err.h"

/** Init netif + wifi STA. Posts SIPWIRE_NET_EVENT. A lost link is retried at
 *  once, then with backoff (1 s → 30 s; 2 s max during a call) forever. Each
 *  connect joins the strongest AP of the SSID; while connected, a weak RSSI
 *  starts a background scan and a clearly stronger AP of the same SSID is
 *  joined (CONFIG_SIPWIRE_WIFI_ROAM; 802.11v steering: SIPWIRE_WIFI_BTM).
 *  Picks up new credentials on SETTINGS_EVENT_CHANGED. Console: `wlan`. */
esp_err_t net_init(void);

/** Connect with explicit credentials (does not persist). */
esp_err_t net_connect(const char *ssid, const char *pass);

bool net_is_connected(void);

/** Current IPv4 as dotted string; "" if none. */
const char *net_ip_str(void);

/** Low-latency mode for calls: Wi-Fi power save off (WIFI_PS_NONE) while
 *  enabled, back to the idle mode when disabled (WIFI_PS_MAX_MODEM with
 *  CONFIG_SIPWIRE_WIFI_LISTEN_INTERVAL > 1, else WIFI_PS_MIN_MODEM). Power save
 *  delivers RTP in bursts at beacon wakeups, which underruns the jitter buffer. The phone
 *  enables it for as long as any call exists (ringing, active, held, two
 *  calls). Idempotent; only a change reaches the driver. While enabled the
 *  RSSI is sampled every 2 s (also without a connection) for the call roam
 *  threshold and the weak-signal warning (NET_EVENT_LINK_WARN). */
void net_set_low_latency(bool enable);
