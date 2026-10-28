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
