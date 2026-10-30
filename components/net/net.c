#include "net.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "console.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "sdkconfig.h"
#if CONFIG_SIPWIRE_QEMU
#include "esp_eth.h"
#include "esp_eth_mac_openeth.h"
#include "esp_system.h"
#endif
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "net_policy.h"
#include "settings.h"
#include "sipwire_events.h"

static const char *TAG = "net";

#define BACKOFF_MIN_MS      1000
#define BACKOFF_MAX_MS      30000
#define BACKOFF_CALL_MAX_MS 2000            /* while a call exists: keep trying */
#define BTM_SAFETY_MS       3000            /* AP-steered roam: the supplicant reconnects */
#define RSSI_PERIOD_US      (10 * 1000 * 1000)
#define RSSI_CALL_PERIOD_US (2 * 1000 * 1000)   /* range warning / roam trigger during calls */
#define RSSI_POST_MS        9500            /* NET_EVENT_RSSI stays ~10 s apart (UI refresh) */
#define IP_SLOTS            4

/* bool Kconfig options as 0/1 (an "n" option is not defined at all) */
#ifdef CONFIG_SIPWIRE_WIFI_ROAM
#define ROAM_BUILT 1
#else
#define ROAM_BUILT 0
#endif
#ifdef CONFIG_SIPWIRE_WIFI_BTM
#define BTM_ON 1
#else
#define BTM_ON 0
#endif
#ifdef CONFIG_SIPWIRE_WIFI_FT
#define FT_ON 1
#else
#define FT_ON 0
#endif

/* Scans: active, 30 ms per empty channel, 70 ms where an AP answered (the IDF roaming app's
 * values; the driver default is a flat 120 ms). Also used by the connect scan, so a full
 * 2.4 GHz scan takes ~0.5-0.9 s instead of ~1.4 s. Roam scans while connected go back to the
 * home channel for 30 ms between channels (the AP buffers our frames meanwhile). */
#define SCAN_ACTIVE_MIN_MS  30
#define SCAN_ACTIVE_MAX_MS  70
#define SCAN_ROAM_MAX_MS    60
#define SCAN_HOME_DWELL_MS  30

static SemaphoreHandle_t s_lock;          /* guards everything below except the atomics */
static esp_timer_handle_t s_retry_timer;
static esp_timer_handle_t s_rssi_timer;
static uint64_t s_rssi_period_us;         /* 0 = stopped */
static bool s_started;
static char s_ssid[sizeof(((settings_t *)0)->wifi_ssid)];
static char s_pass[sizeof(((settings_t *)0)->wifi_pass)];
static uint32_t s_backoff_ms = BACKOFF_MIN_MS;
static bool s_associated;                 /* between STA_CONNECTED and STA_DISCONNECTED */
static bool s_expect_leave;               /* we called esp_wifi_disconnect() (creds switch, roam) */
static bool s_low_latency;                /* WIFI_PS_NONE (call) instead of NET_IDLE_PS */
static uint8_t s_bssid[6];                /* current / last AP */
static uint8_t s_channel;
static int s_rssi = NET_RSSI_NONE;        /* last sample */
static uint64_t s_rssi_posted_ms;
static uint64_t s_down_us;                /* link lost (had an association), 0 = up */
static uint64_t s_assoc_us;               /* association after s_down_us */
static uint32_t s_links_lost;
static uint32_t s_last_outage_ms, s_last_assoc_ms;
/* roaming */
#if CONFIG_SIPWIRE_WIFI_ROAM
static net_roam_t s_roam;
#endif
static bool s_roam_on = ROAM_BUILT;
static bool s_scanning;                   /* our roam scan in flight */
static bool s_scan_verbose;               /* console `wlan scan`: list every AP */
static bool s_roaming;                    /* targeted reconnect to s_roam_bssid in progress */
static uint8_t s_roam_bssid[6];
static uint8_t s_roam_channel;
static uint32_t s_scans, s_roams, s_roam_fails, s_btm_roams;
/* weak-signal warning */
static net_warn_t s_warn;
#if CONFIG_SIPWIRE_WIFI_RANGE_WARN
static int s_warn_dbm = CONFIG_SIPWIRE_WIFI_WARN_RSSI;    /* console `wlan warn` overrides */
#endif

/* Idle Wi-Fi power save. Listen interval 1 = WIFI_PS_MIN_MODEM: the radio wakes for every DTIM
 * beacon. Above 1 = WIFI_PS_MAX_MODEM: it wakes every CONFIG_SIPWIRE_WIFI_LISTEN_INTERVAL
 * beacons (the interval is announced in the association request, so it is set in the STA
 * config before connecting, sta_config). An incoming INVITE then waits in the AP for up to
 * listen_interval x beacon interval (3 x 102.4 ms = 0.3 s by default). Calls: WIFI_PS_NONE. */
#if CONFIG_SIPWIRE_WIFI_LISTEN_INTERVAL > 1
#define NET_IDLE_PS     WIFI_PS_MAX_MODEM
#else
#define NET_IDLE_PS     WIFI_PS_MIN_MODEM
#endif

static atomic_bool s_connected;

/* net_ip_str() hands out a pointer without holding a lock, so a published string is never
 * rewritten in place: each GOT_IP fills the next slot of a small ring, then swaps the pointer.
 * A reader would have to sit on the pointer across IP_SLOTS address changes to see a torn read. */
static char s_ip_slots[IP_SLOTS][16];
static unsigned s_ip_next;
static _Atomic(const char *) s_ip_ptr = "";

static uint64_t now_ms(void)
{
    return (uint64_t)esp_timer_get_time() / 1000;
}

#if CONFIG_SIPWIRE_WIFI_ROAM
static const net_roam_cfg_t ROAM_CFG = {
    .idle_dbm = CONFIG_SIPWIRE_WIFI_ROAM_RSSI,
    .call_dbm = CONFIG_SIPWIRE_WIFI_ROAM_CALL_RSSI,
    .delta_db = CONFIG_SIPWIRE_WIFI_ROAM_DELTA,
    .interval_ms = CONFIG_SIPWIRE_WIFI_ROAM_SCAN_INTERVAL * 1000u,
    .max_backoff = 3,
};
#endif

static void net_post(net_event_id_t id, const void *data, size_t size)
{
    esp_err_t err = esp_event_post(SIPWIRE_NET_EVENT, id, data, size, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "post event %d failed: %s", (int)id, esp_err_to_name(err));
    }
}

/* Caller holds s_lock. STA config from the credentials; bssid != NULL: that AP only (roam). */
static void sta_config(wifi_config_t *cfg, const uint8_t *bssid, uint8_t channel)
{
    memset(cfg, 0, sizeof(*cfg));
    size_t ssid_len = strnlen(s_ssid, sizeof(cfg->sta.ssid));
    size_t pass_len = strnlen(s_pass, sizeof(cfg->sta.password) - 1);
    memcpy(cfg->sta.ssid, s_ssid, ssid_len);
    memcpy(cfg->sta.password, s_pass, pass_len);
    cfg->sta.threshold.authmode = pass_len > 0 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    cfg->sta.pmf_cfg.capable = true;
    cfg->sta.listen_interval = CONFIG_SIPWIRE_WIFI_LISTEN_INTERVAL;    /* only WIFI_PS_MAX_MODEM uses it */
    /* Every connect: the strongest AP of the SSID, not the first one found (mesh, extenders). */
    cfg->sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    cfg->sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    cfg->sta.btm_enabled = BTM_ON;
    cfg->sta.ft_enabled = FT_ON;
    if (bssid) {
        /* Roam: the scan found it, go straight there (one channel, first match). With BTM on,
         * the supplicant clears bssid/channel again at the next (dis)connection. */
        cfg->sta.scan_method = WIFI_FAST_SCAN;
        cfg->sta.bssid_set = true;
        memcpy(cfg->sta.bssid, bssid, 6);
        cfg->sta.channel = channel;
    }
}

/* Caller holds s_lock. */
static void connect_now(const uint8_t *bssid, uint8_t channel)
{
    wifi_config_t cfg;
    sta_config(&cfg, bssid, channel);
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "set_config: %s", esp_err_to_name(err));
    }
    net_post(NET_EVENT_CONNECTING, NULL, 0);
    err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_connect: %s", esp_err_to_name(err));
    }
}

/* Caller holds s_lock. Restarting the one-shot timer makes this idempotent. */
static void schedule_retry(uint32_t delay_ms)
{
    esp_timer_stop(s_retry_timer);
    esp_timer_start_once(s_retry_timer, (uint64_t)delay_ms * 1000);
}

static void retry_cb(void *arg)
{
    (void)arg;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_started && s_ssid[0] != '\0' && !atomic_load(&s_connected) && !s_associated && !s_roaming) {
        connect_now(NULL, 0);
    }
    xSemaphoreGive(s_lock);
}

/* Caller holds s_lock. RSSI sampling runs while associated (10 s; 2 s during a call) and
 * during a call also without a connection (the range warning counts that as weak). */
static void rssi_timer_update(void)
{
    uint64_t want = 0;
    if (s_low_latency) {
        want = RSSI_CALL_PERIOD_US;
    } else if (s_associated) {
        want = RSSI_PERIOD_US;
    }
    if (want == s_rssi_period_us) {
        return;
    }
    esp_timer_stop(s_rssi_timer);
    if (want) {
        esp_timer_start_periodic(s_rssi_timer, want);
    }
    s_rssi_period_us = want;
}

/* Caller holds s_lock. */
static void roam_scan_start(bool verbose)
{
    wifi_scan_config_t sc = {
        .ssid = (uint8_t *)s_ssid,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active = { .min = SCAN_ACTIVE_MIN_MS, .max = SCAN_ROAM_MAX_MS },
        .home_chan_dwell_time = SCAN_HOME_DWELL_MS,
    };
    esp_err_t err = esp_wifi_scan_start(&sc, false);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "roam scan: %s", esp_err_to_name(err));
#if CONFIG_SIPWIRE_WIFI_ROAM
        s_roam.next_scan_ms = now_ms() + ROAM_CFG.interval_ms;
#endif
        return;
    }
    s_scanning = true;
    s_scan_verbose = verbose;
    s_scans++;
    ESP_LOGI(TAG, "roam scan (%d dBm%s)", s_rssi, s_low_latency ? ", call" : "");
}

/* Caller holds s_lock. Our roam scan finished: roam if a clearly stronger AP showed up. */
static void roam_scan_done(void)
{
    bool verbose = s_scan_verbose;
    s_scanning = false;
    s_scan_verbose = false;

    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    net_roam_scan_t sc;
    net_roam_scan_init(&sc);
    wifi_ap_record_t rec;
    for (uint16_t i = 0; i < n && esp_wifi_scan_get_ap_record(&rec) == ESP_OK; i++) {
        if (strncmp((const char *)rec.ssid, s_ssid, sizeof(rec.ssid)) != 0) {
            continue;
        }
        bool cur = s_associated && memcmp(rec.bssid, s_bssid, 6) == 0;
        if (verbose) {
            ESP_LOGI(TAG, "  " MACSTR " ch %2u %4d dBm%s", MAC2STR(rec.bssid), rec.primary, rec.rssi,
                     cur ? " (current)" : "");
        }
        net_roam_scan_add(&sc, s_associated ? s_bssid : NULL, rec.bssid, rec.rssi, rec.primary);
    }
    esp_wifi_clear_ap_list();

    if (!s_associated || !atomic_load(&s_connected)) {
        return;                     /* the link went away meanwhile: the reconnect picks the best */
    }
    int cur = NET_RSSI_NONE;
    int v;
    if (esp_wifi_sta_get_rssi(&v) == ESP_OK) {
        cur = v;
    }
#if CONFIG_SIPWIRE_WIFI_ROAM
    bool go = net_roam_evaluate(&s_roam, &ROAM_CFG, &sc, cur, now_ms());
#else
    bool go = false;
#endif
    go = go && s_roam_on;
    if (sc.rssi == NET_RSSI_NONE) {
        ESP_LOGI(TAG, "roam scan: no other AP (current %d dBm, scan %d dBm)", cur, sc.cur_rssi);
    } else {
        ESP_LOGI(TAG, "roam scan: best other " MACSTR " ch %u %d dBm, current %d dBm (scan %d): %s",
                 MAC2STR(sc.bssid), sc.channel, sc.rssi, cur, sc.cur_rssi, go ? "roaming" : "staying");
    }
    if (!go) {
        return;
    }
    memcpy(s_roam_bssid, sc.bssid, 6);
    s_roam_channel = sc.channel;
    s_roaming = true;
    s_expect_leave = true;          /* the DISCONNECTED handler joins s_roam_bssid at once */
    s_roams++;
    ESP_LOGI(TAG, "roam: " MACSTR " -> " MACSTR " ch %u%s", MAC2STR(s_bssid), MAC2STR(s_roam_bssid),
             s_roam_channel, s_low_latency ? " (during a call)" : "");
    esp_err_t err = esp_wifi_disconnect();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "roam disconnect: %s", esp_err_to_name(err));
        s_roaming = false;
        s_expect_leave = false;
    }
}

static void rssi_cb(void *arg)
{
    (void)arg;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(10)) != pdTRUE) {
        return;                     /* a (re)configuration holds it: skip one sample */
    }
    uint64_t now = now_ms();
    int rssi = NET_RSSI_NONE;
    int v;
    if (s_associated && esp_wifi_sta_get_rssi(&v) == ESP_OK) {
        rssi = v;
    }
    s_rssi = rssi;
    if (rssi != NET_RSSI_NONE && now - s_rssi_posted_ms >= RSSI_POST_MS) {
        int8_t ev = (int8_t)rssi;
        net_post(NET_EVENT_RSSI, &ev, sizeof(ev));
        s_rssi_posted_ms = now;
    }

#if CONFIG_SIPWIRE_WIFI_RANGE_WARN
    const net_warn_cfg_t wc = {
        .warn_dbm = s_warn_dbm,
        .hyst_db = CONFIG_SIPWIRE_WIFI_WARN_HYST,
        .repeat_ms = CONFIG_SIPWIRE_WIFI_WARN_INTERVAL * 1000u,
        .enter_samples = 2,
    };
    net_warn_action_t a = net_warn_update(&s_warn, &wc, s_low_latency, rssi, now);
    if (a != NET_WARN_NONE) {
        net_event_link_warn_t ev = { .rssi = (int8_t)rssi, .weak = a != NET_WARN_CLEAR };
        if (a == NET_WARN_ENTER) {
            ESP_LOGW(TAG, "weak signal during a call: %d dBm (below %d), warning", rssi, s_warn_dbm);
        } else if (a == NET_WARN_CLEAR) {
            ESP_LOGI(TAG, "signal back to %d dBm", rssi);
        } else {
            ESP_LOGI(TAG, "weak signal: %d dBm", rssi);
        }
        net_post(NET_EVENT_LINK_WARN, &ev, sizeof(ev));
    }
#endif

#if CONFIG_SIPWIRE_WIFI_ROAM
    if (s_roam_on && atomic_load(&s_connected) && !s_scanning && !s_roaming &&
        net_roam_scan_due(&s_roam, &ROAM_CFG, s_low_latency, rssi, now)) {
        roam_scan_start(false);
    }
#endif
    xSemaphoreGive(s_lock);
}

/* Caller holds s_lock. Installs the credentials and (re)connects, or goes idle if SSID is empty. */
static esp_err_t apply_creds(const char *ssid, const char *pass)
{
    size_t ssid_len = strlen(ssid);
    size_t pass_len = strlen(pass);
    ESP_RETURN_ON_FALSE(ssid_len <= sizeof(((wifi_sta_config_t *)0)->ssid) &&
                        pass_len < sizeof(((wifi_sta_config_t *)0)->password),
                        ESP_ERR_INVALID_ARG, TAG, "ssid/password too long");

    memmove(s_ssid, ssid, ssid_len + 1);   /* ssid may alias s_ssid */
    memmove(s_pass, pass, pass_len + 1);
    s_backoff_ms = BACKOFF_MIN_MS;
    s_roaming = false;
    esp_timer_stop(s_retry_timer);

    if (!s_started) {
        return ESP_OK;      /* picked up on WIFI_EVENT_STA_START */
    }
    wifi_config_t cfg;
    sta_config(&cfg, NULL, 0);
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &cfg), TAG, "set_config");

    if (s_associated) {
        /* Reconnect from the DISCONNECTED handler once the old association is gone. The
         * disconnect reason can't tell us apart from the AP leaving, so use a flag. */
        s_expect_leave = true;
        esp_wifi_disconnect();
    } else if (ssid_len > 0) {
        esp_wifi_disconnect();  /* abort an in-flight attempt; no-op error if idle */
        connect_now(NULL, 0);
    }
    if (ssid_len == 0) {
        ESP_LOGI(TAG, "no SSID configured, idle");
    } else {
        ESP_LOGI(TAG, "connecting to '%s'", s_ssid);
    }
    return ESP_OK;
}

/* Caller holds s_lock. */
static void on_disconnected(const wifi_event_sta_disconnected_t *d)
{
    bool was_up = s_associated;
    net_event_disc_t ev = { .reason = d->reason };
    atomic_store(&s_connected, false);
    atomic_store(&s_ip_ptr, "");
    s_associated = false;
    if (s_scanning) {
        esp_wifi_scan_stop();
        s_scanning = false;
    }
    if (was_up) {
        s_down_us = esp_timer_get_time();
        s_assoc_us = 0;
        s_links_lost++;
    }
    rssi_timer_update();
    net_post(NET_EVENT_DISCONNECTED, &ev, sizeof(ev));

    if (s_expect_leave) {
        s_expect_leave = false;
        ESP_LOGI(TAG, "disconnected (%s), reconnecting", s_roaming ? "roam" : "new credentials");
        if (s_ssid[0] != '\0') {
            connect_now(s_roaming ? s_roam_bssid : NULL, s_roam_channel);
        }
        return;
    }
    if (s_roaming) {
        /* the targeted join failed: any AP of the SSID, strongest first */
        s_roaming = false;
        s_roam_fails++;
        ESP_LOGW(TAG, "roam to " MACSTR " failed (reason %d), reconnecting", MAC2STR(s_roam_bssid), d->reason);
        connect_now(NULL, 0);
        return;
    }
    if (was_up && (d->reason == WIFI_REASON_BSS_TRANSITION_DISASSOC || d->reason == WIFI_REASON_ROAMING)) {
        /* 802.11v steering (or a driver roam): the supplicant is already joining the AP the
         * network named. Only a safety net here; retry_cb does nothing once associated. */
        s_btm_roams++;
        ESP_LOGI(TAG, "BSS transition (reason %d)", d->reason);
        schedule_retry(BTM_SAFETY_MS);
        return;
    }
    ESP_LOGW(TAG, "disconnected, reason %d", d->reason);
    if (s_ssid[0] == '\0') {
        return;
    }
    if (was_up) {
        connect_now(NULL, 0);       /* lost an established link: try again at once */
        return;
    }
    ESP_LOGI(TAG, "retry in %u ms", (unsigned)s_backoff_ms);
    schedule_retry(s_backoff_ms);
    uint32_t cap = s_low_latency ? BACKOFF_CALL_MAX_MS : BACKOFF_MAX_MS;
    s_backoff_ms = s_backoff_ms * 2 > cap ? cap : s_backoff_ms * 2;
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    switch (id) {
    case WIFI_EVENT_STA_START: {
        s_started = true;
        const wifi_scan_default_params_t sp = {
            .scan_time.active = { .min = SCAN_ACTIVE_MIN_MS, .max = SCAN_ACTIVE_MAX_MS },
            .home_chan_dwell_time = SCAN_HOME_DWELL_MS,
        };
        esp_err_t err = esp_wifi_set_scan_parameters(&sp);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "scan parameters: %s", esp_err_to_name(err));
        }
        /* Re-run so the config reaches the driver and the first connect happens. */
        apply_creds(s_ssid, s_pass);
        break;
    }
    case WIFI_EVENT_STA_CONNECTED: {
        const wifi_event_sta_connected_t *c = data;
        s_associated = true;
        memcpy(s_bssid, c->bssid, 6);
        s_channel = c->channel;
        if (s_down_us) {
            s_assoc_us = esp_timer_get_time();
        }
        ESP_LOGI(TAG, "associated with " MACSTR " ch %u%s", MAC2STR(c->bssid), c->channel,
                 s_roaming ? " (roam)" : "");
        s_roaming = false;
        rssi_timer_update();
        break;
    }
    case WIFI_EVENT_STA_DISCONNECTED:
        on_disconnected(data);
        break;
    case WIFI_EVENT_SCAN_DONE:
        if (s_scanning) {
            roam_scan_done();
        } else {
            esp_wifi_clear_ap_list();   /* a scan stopped on disconnect: nobody else scans */
        }
        break;
    default:
        break;
    }
    xSemaphoreGive(s_lock);
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)id;
    const ip_event_got_ip_t *got = data;

    net_event_ip_t ev = { .ip = got->ip_info.ip.addr };
    esp_ip4addr_ntoa(&got->ip_info.ip, ev.ip_str, sizeof(ev.ip_str));

    xSemaphoreTake(s_lock, portMAX_DELAY);
    char *slot = s_ip_slots[s_ip_next++ % IP_SLOTS];
    memcpy(slot, ev.ip_str, sizeof(ev.ip_str));
    atomic_store(&s_ip_ptr, slot);
    atomic_store(&s_connected, true);
    s_backoff_ms = BACKOFF_MIN_MS;
#if CONFIG_SIPWIRE_WIFI_ROAM
    net_roam_reset(&s_roam, &ROAM_CFG, now_ms());
#endif
    if (s_down_us) {
        /* the audio gap of a roam / reconnect: link down until the IP is back */
        uint64_t t = esp_timer_get_time();
        s_last_outage_ms = (uint32_t)((t - s_down_us) / 1000);
        s_last_assoc_ms = s_assoc_us ? (uint32_t)((s_assoc_us - s_down_us) / 1000) : 0;
        s_down_us = 0;
        ESP_LOGI(TAG, "got ip %s%s, link was down %u ms (associated after %u ms)", ev.ip_str,
                 got->ip_changed ? " (changed)" : "", (unsigned)s_last_outage_ms, (unsigned)s_last_assoc_ms);
    } else {
        ESP_LOGI(TAG, "got ip %s", ev.ip_str);
    }
    xSemaphoreGive(s_lock);

    net_post(NET_EVENT_GOT_IP, &ev, sizeof(ev));
}

static void on_settings_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)data;
    if (id != SETTINGS_EVENT_CHANGED) {
        return;
    }
    char ssid[sizeof(s_ssid)], pass[sizeof(s_pass)];
    SETTINGS_GET(wifi_ssid, &ssid);
    SETTINGS_GET(wifi_pass, &pass);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (strcmp(ssid, s_ssid) != 0 || strcmp(pass, s_pass) != 0) {
        apply_creds(ssid, pass);
    }
    xSemaphoreGive(s_lock);
    memset(pass, 0, sizeof(pass));
}

/* ------------------------------------------------------------------ console `wlan` */

static void print_status(void)
{
    wifi_ps_type_t ps = WIFI_PS_NONE;
    esp_wifi_get_ps(&ps);
    int v;
    if (s_associated && esp_wifi_sta_get_rssi(&v) == ESP_OK) {
        s_rssi = v;
    }
    if (s_associated) {
        printf("ssid \"%s\" bssid " MACSTR " ch %u, %d dBm, ip %s\n", s_ssid, MAC2STR(s_bssid), s_channel,
               s_rssi, atomic_load(&s_connected) ? net_ip_str() : "(none)");
    } else {
        printf("not associated (ssid \"%s\")%s\n", s_ssid, s_roaming ? ", roaming" : "");
    }
    printf("rssi sampled every %u s%s, power save %s\n", (unsigned)(s_rssi_period_us / 1000000),
           s_rssi_period_us ? "" : " (stopped)",
           ps == WIFI_PS_NONE ? "off (call)" : "on");
    printf("links lost %u, last outage %u ms (associated after %u ms)\n", (unsigned)s_links_lost,
           (unsigned)s_last_outage_ms, (unsigned)s_last_assoc_ms);
#if CONFIG_SIPWIRE_WIFI_ROAM
    uint64_t now = now_ms();
    printf("roaming %s: scan below %d dBm idle / %d dBm call, +%d dB to move, every %u s (x%u now)\n",
           s_roam_on ? "on" : "off", ROAM_CFG.idle_dbm, ROAM_CFG.call_dbm, ROAM_CFG.delta_db,
           (unsigned)(ROAM_CFG.interval_ms / 1000), 1u << s_roam.backoff);
    printf("  scans %u, roams %u (failed %u), AP-steered %u, next scan %s%u s\n", (unsigned)s_scans,
           (unsigned)s_roams, (unsigned)s_roam_fails, (unsigned)s_btm_roams,
           s_roam.next_scan_ms > now ? "in " : "allowed, ",
           (unsigned)(s_roam.next_scan_ms > now ? (s_roam.next_scan_ms - now) / 1000 : 0));
#else
    printf("roaming: built without CONFIG_SIPWIRE_WIFI_ROAM (AP-steered %u)\n", (unsigned)s_btm_roams);
#endif
    printf("802.11v BTM %s, 802.11r FT %s\n", BTM_ON ? "on" : "off",
           FT_ON ? "on" : "off");
#if CONFIG_SIPWIRE_WIFI_RANGE_WARN
    printf("range warning below %d dBm (back at %d), every %d s: %s\n", s_warn_dbm,
           s_warn_dbm + CONFIG_SIPWIRE_WIFI_WARN_HYST, CONFIG_SIPWIRE_WIFI_WARN_INTERVAL,
           !s_low_latency ? "idle (no call)" : s_warn.weak ? "WEAK" : "ok");
#else
    printf("range warning: built without CONFIG_SIPWIRE_WIFI_RANGE_WARN\n");
#endif
}

static int cmd_wlan(int argc, char **argv)
{
    int ret = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (argc == 1) {
        print_status();
    } else if (strcmp(argv[1], "scan") == 0) {
        if (!atomic_load(&s_connected) || s_scanning || s_roaming) {
            printf("not now (%s)\n", !atomic_load(&s_connected) ? "not connected" : "busy");
        } else {
            roam_scan_start(true);
            printf("scanning; APs and the decision follow in the log\n");
        }
    } else if (strcmp(argv[1], "roam") == 0 && argc == 3 &&
               (strcmp(argv[2], "on") == 0 || strcmp(argv[2], "off") == 0)) {
        s_roam_on = strcmp(argv[2], "on") == 0;
        printf("roaming %s until reboot%s\n", s_roam_on ? "on" : "off",
               ROAM_BUILT ? "" : " (automatic scans not built in)");
#if CONFIG_SIPWIRE_WIFI_RANGE_WARN
    } else if (strcmp(argv[1], "warn") == 0 && argc == 3) {
        int dbm = strcmp(argv[2], "default") == 0 ? CONFIG_SIPWIRE_WIFI_WARN_RSSI : atoi(argv[2]);
        if (dbm < -100 || dbm > -1) {
            printf("dBm -100..-1 or default\n");
            ret = 1;
        } else {
            s_warn_dbm = dbm;
            printf("range warning below %d dBm until reboot (during calls)\n", dbm);
        }
#endif
    } else {
        printf("usage: wlan [scan | roam on|off | warn <dBm>|default]\n");
        ret = 1;
    }
    xSemaphoreGive(s_lock);
    return ret;
}

#if CONFIG_SIPWIRE_QEMU
/* QEMU smoke-test build: OpenCores Ethernet (slirp) stands in for Wi-Fi. */
static void on_eth_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)id;
    const ip_event_got_ip_t *got = data;
    net_event_ip_t ev = { .ip = got->ip_info.ip.addr };
    esp_ip4addr_ntoa(&got->ip_info.ip, ev.ip_str, sizeof(ev.ip_str));
    char *slot = s_ip_slots[s_ip_next++ % IP_SLOTS];
    memcpy(slot, ev.ip_str, sizeof(ev.ip_str));
    atomic_store(&s_ip_ptr, slot);
    atomic_store(&s_connected, true);
    ESP_LOGI(TAG, "eth got ip %s", ev.ip_str);
    net_post(NET_EVENT_GOT_IP, &ev, sizeof(ev));
}

static esp_eth_handle_t s_eth;

/* esp_restart() (e.g. after an OTA update): stop the emulated OpenCores EMAC
 * first. Left running across the software reset it kept receiving while the
 * next boot came up, which corrupted memory: a Guru Meditation in the new
 * image or QEMU itself aborting ("malloc(): unsorted double linked list
 * corrupted") right after the Ethernet driver started again. */
static void net_qemu_shutdown(void)
{
    if (s_eth) {
        esp_eth_stop(s_eth);
    }
}

static esp_err_t net_init_qemu(void)
{
    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *netif = esp_netif_new(&netif_cfg);
    ESP_RETURN_ON_FALSE(netif != NULL, ESP_FAIL, TAG, "eth netif");
    eth_mac_config_t mac_cfg = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_cfg = ETH_PHY_DEFAULT_CONFIG();
    phy_cfg.autonego_timeout_ms = 100;
    esp_eth_mac_t *mac = esp_eth_mac_new_openeth(&mac_cfg);
    esp_eth_phy_t *phy = esp_eth_phy_new_generic(&phy_cfg);
    esp_eth_config_t eth_cfg = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t eth = NULL;
    ESP_RETURN_ON_ERROR(esp_eth_driver_install(&eth_cfg, &eth), TAG, "eth install");
    ESP_RETURN_ON_ERROR(esp_netif_attach(netif, esp_eth_new_netif_glue(eth)), TAG, "eth attach");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, on_eth_ip, NULL), TAG,
                        "register eth ip handler");
    net_post(NET_EVENT_CONNECTING, NULL, 0);
    s_eth = eth;
    ESP_RETURN_ON_ERROR(esp_register_shutdown_handler(net_qemu_shutdown), TAG, "shutdown handler");
    return esp_eth_start(eth);
}
#endif

esp_err_t net_init(void)
{
    ESP_RETURN_ON_FALSE(s_lock == NULL, ESP_ERR_INVALID_STATE, TAG, "already initialized");
    s_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_lock != NULL, ESP_ERR_NO_MEM, TAG, "no mem for mutex");
#if CONFIG_SIPWIRE_QEMU
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init");
    return net_init_qemu();
#endif

    const esp_timer_create_args_t retry_args = { .callback = retry_cb, .name = "net_retry" };
    const esp_timer_create_args_t rssi_args = { .callback = rssi_cb, .name = "net_rssi" };
    ESP_RETURN_ON_ERROR(esp_timer_create(&retry_args, &s_retry_timer), TAG, "retry timer");
    ESP_RETURN_ON_ERROR(esp_timer_create(&rssi_args, &s_rssi_timer), TAG, "rssi timer");

    SETTINGS_GET(wifi_ssid, &s_ssid);
    SETTINGS_GET(wifi_pass, &s_pass);

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init");
    esp_netif_t *netif = esp_netif_create_default_wifi_sta();
    ESP_RETURN_ON_FALSE(netif != NULL, ESP_FAIL, TAG, "create sta netif");

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init_cfg), TAG, "wifi init");
    /* Credentials live in settings (NVS "sipwire"); keep the wifi driver from keeping its own copy. */
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "wifi storage");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "wifi mode");

    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL), TAG,
                        "register wifi handler");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_ip_event, NULL), TAG,
                        "register ip handler");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(SIPWIRE_SETTINGS_EVENT, ESP_EVENT_ANY_ID,
                                                   on_settings_event, NULL),
                        TAG, "register settings handler");

    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start");
    /* Modem sleep while idle (NET_IDLE_PS above), enough for SIP signalling. Calls switch to
     * PS_NONE (net_set_low_latency): during media the beacon-paced bursts stutter the audio. */
    ESP_RETURN_ON_ERROR(esp_wifi_set_ps(NET_IDLE_PS), TAG, "wifi ps");

    const esp_console_cmd_t cmd = {
        .command = "wlan",
        .help = "Wi-Fi link: AP, RSSI, outages, roaming and range-warning state. `wlan scan`: look "
                "for a stronger AP now (roams if one is better by the margin). `wlan roam on|off`, "
                "`wlan warn <dBm>|default`: until reboot",
        .hint = "[scan | roam on|off | warn <dBm>|default]",
        .func = cmd_wlan,
    };
    ESP_RETURN_ON_ERROR(console_register_command(&cmd), TAG, "register wlan");

    ESP_LOGI(TAG, "wifi sta started%s", s_ssid[0] ? "" : " (no SSID configured, idle)");
    return ESP_OK;
}

esp_err_t net_connect(const char *ssid, const char *pass)
{
    ESP_RETURN_ON_FALSE(ssid != NULL && pass != NULL, ESP_ERR_INVALID_ARG, TAG, "null credentials");
    ESP_RETURN_ON_FALSE(s_lock != NULL, ESP_ERR_INVALID_STATE, TAG, "not initialized");

    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = apply_creds(ssid, pass);
    xSemaphoreGive(s_lock);
    return err;
}

bool net_is_connected(void)
{
    return atomic_load(&s_connected);
}

const char *net_ip_str(void)
{
    return atomic_load(&s_ip_ptr);
}

void net_set_low_latency(bool enable)
{
#if CONFIG_SIPWIRE_QEMU
    (void)enable;
#else
    /* Called on every call state change: only touch the driver when the mode changes. A
     * failed switch leaves s_low_latency as it was, so the next call retries. */
    if (s_lock == NULL) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (enable != s_low_latency) {
        esp_err_t err = esp_wifi_set_ps(enable ? WIFI_PS_NONE : NET_IDLE_PS);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "wifi ps: %s", esp_err_to_name(err));
        } else {
            s_low_latency = enable;
            ESP_LOGI(TAG, "power save %s", enable ? "off (call)" : "on (modem sleep)");
            net_warn_reset(&s_warn);
            rssi_timer_update();    /* 2 s samples for the range warning / call roaming */
        }
    }
    xSemaphoreGive(s_lock);
#endif
}
