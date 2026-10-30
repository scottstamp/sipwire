#include "net.h"

#include <stdatomic.h>
#include <string.h>
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
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
#include "settings.h"
#include "sipwire_events.h"

static const char *TAG = "net";

#define BACKOFF_MIN_MS  1000
#define BACKOFF_MAX_MS  30000
#define RSSI_PERIOD_US  (10 * 1000 * 1000)
#define IP_SLOTS        4

static SemaphoreHandle_t s_lock;          /* guards everything below except the atomics */
static esp_timer_handle_t s_retry_timer;
static esp_timer_handle_t s_rssi_timer;
static bool s_started;
static char s_ssid[sizeof(((settings_t *)0)->wifi_ssid)];
static char s_pass[sizeof(((settings_t *)0)->wifi_pass)];
static uint32_t s_backoff_ms = BACKOFF_MIN_MS;
static bool s_associated;                 /* between STA_CONNECTED and STA_DISCONNECTED */
static bool s_expect_leave;               /* we called esp_wifi_disconnect() to switch creds */
#if !CONFIG_SIPWIRE_QEMU
static bool s_low_latency;                /* WIFI_PS_NONE (call) instead of WIFI_PS_MIN_MODEM */
#endif

static atomic_bool s_connected;

/* net_ip_str() hands out a pointer without holding a lock, so a published string is never
 * rewritten in place: each GOT_IP fills the next slot of a small ring, then swaps the pointer.
 * A reader would have to sit on the pointer across IP_SLOTS address changes to see a torn read. */
static char s_ip_slots[IP_SLOTS][16];
static unsigned s_ip_next;
static _Atomic(const char *) s_ip_ptr = "";

static void net_post(net_event_id_t id, const void *data, size_t size)
{
    esp_err_t err = esp_event_post(SIPWIRE_NET_EVENT, id, data, size, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "post event %d failed: %s", (int)id, esp_err_to_name(err));
    }
}

/* Caller holds s_lock. */
static void connect_now(void)
{
    net_post(NET_EVENT_CONNECTING, NULL, 0);
    esp_err_t err = esp_wifi_connect();
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
    if (s_started && s_ssid[0] != '\0' && !atomic_load(&s_connected)) {
        connect_now();
    }
    xSemaphoreGive(s_lock);
}

static void rssi_cb(void *arg)
{
    (void)arg;
    int rssi;
    if (esp_wifi_sta_get_rssi(&rssi) == ESP_OK) {
        int8_t v = (int8_t)rssi;
        net_post(NET_EVENT_RSSI, &v, sizeof(v));
    }
}

/* Caller holds s_lock. Installs the credentials and (re)connects, or goes idle if SSID is empty. */
static esp_err_t apply_creds(const char *ssid, const char *pass)
{
    wifi_config_t cfg = { 0 };
    size_t ssid_len = strlen(ssid);
    size_t pass_len = strlen(pass);
    ESP_RETURN_ON_FALSE(ssid_len <= sizeof(cfg.sta.ssid) && pass_len < sizeof(cfg.sta.password),
                        ESP_ERR_INVALID_ARG, TAG, "ssid/password too long");

    memcpy(cfg.sta.ssid, ssid, ssid_len);
    memcpy(cfg.sta.password, pass, pass_len);
    cfg.sta.threshold.authmode = pass_len > 0 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    cfg.sta.pmf_cfg.capable = true;

    memmove(s_ssid, ssid, ssid_len + 1);   /* ssid may alias s_ssid */
    memmove(s_pass, pass, pass_len + 1);
    s_backoff_ms = BACKOFF_MIN_MS;
    esp_timer_stop(s_retry_timer);

    if (!s_started) {
        return ESP_OK;      /* picked up on WIFI_EVENT_STA_START */
    }
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &cfg), TAG, "set_config");

    if (s_associated) {
        /* Reconnect from the DISCONNECTED handler once the old association is gone. The
         * disconnect reason can't tell us apart from the AP leaving, so use a flag. */
        s_expect_leave = true;
        esp_wifi_disconnect();
    } else if (ssid_len > 0) {
        esp_wifi_disconnect();  /* abort an in-flight attempt; no-op error if idle */
        connect_now();
    }
    if (ssid_len == 0) {
        ESP_LOGI(TAG, "no SSID configured, idle");
    } else {
        ESP_LOGI(TAG, "connecting to '%s'", s_ssid);
    }
    return ESP_OK;
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    switch (id) {
    case WIFI_EVENT_STA_START:
        s_started = true;
        /* Re-run so the config reaches the driver and the first connect happens. */
        apply_creds(s_ssid, s_pass);
        break;
    case WIFI_EVENT_STA_CONNECTED:
        s_associated = true;
        break;
    case WIFI_EVENT_STA_DISCONNECTED: {
        const wifi_event_sta_disconnected_t *d = data;
        net_event_disc_t ev = { .reason = d->reason };
        atomic_store(&s_connected, false);
        atomic_store(&s_ip_ptr, "");
        esp_timer_stop(s_rssi_timer);
        ESP_LOGW(TAG, "disconnected, reason %d", d->reason);
        net_post(NET_EVENT_DISCONNECTED, &ev, sizeof(ev));
        s_associated = false;
        if (s_expect_leave) {
            s_expect_leave = false;
            if (s_ssid[0] != '\0') {
                connect_now();
            }
        } else if (s_ssid[0] != '\0') {
            ESP_LOGI(TAG, "retry in %u ms", (unsigned)s_backoff_ms);
            schedule_retry(s_backoff_ms);
            s_backoff_ms = s_backoff_ms * 2 > BACKOFF_MAX_MS ? BACKOFF_MAX_MS : s_backoff_ms * 2;
        }
        break;
    }
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
    esp_timer_stop(s_rssi_timer);
    esp_timer_start_periodic(s_rssi_timer, RSSI_PERIOD_US);
    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "got ip %s", ev.ip_str);
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
    settings_t s;
    settings_get(&s);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (strcmp(s.wifi_ssid, s_ssid) != 0 || strcmp(s.wifi_pass, s_pass) != 0) {
        apply_creds(s.wifi_ssid, s.wifi_pass);
    }
    xSemaphoreGive(s_lock);
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

    settings_t s;
    settings_get(&s);
    memcpy(s_ssid, s.wifi_ssid, sizeof(s_ssid));
    memcpy(s_pass, s.wifi_pass, sizeof(s_pass));

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
    /* WIFI_PS_MIN_MODEM while idle: radio sleeps between DTIM beacons only, enough for SIP
     * signalling. Calls switch to PS_NONE (net_set_low_latency): during media the DTIM
     * bursts stutter the audio. MAX_MODEM adds listen-interval latency to incoming INVITEs. */
    ESP_RETURN_ON_ERROR(esp_wifi_set_ps(WIFI_PS_MIN_MODEM), TAG, "wifi ps");

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
        esp_err_t err = esp_wifi_set_ps(enable ? WIFI_PS_NONE : WIFI_PS_MIN_MODEM);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "wifi ps: %s", esp_err_to_name(err));
        } else {
            s_low_latency = enable;
            ESP_LOGI(TAG, "power save %s", enable ? "off (call)" : "on (modem sleep)");
        }
    }
    xSemaphoreGive(s_lock);
#endif
}
