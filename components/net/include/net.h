#pragma once

#include <stdbool.h>
#include "esp_err.h"

/** Init netif + wifi STA. Posts SIPWIRE_NET_EVENT. Reconnects with backoff
 *  (1 s → 30 s) forever. Picks up new credentials on SETTINGS_EVENT_CHANGED. */
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
 *  calls). Idempotent; only a change reaches the driver. */
void net_set_low_latency(bool enable);
