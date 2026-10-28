#include "settings.h"

#include <string.h>
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "sipwire_events.h"

static const char *TAG = "storage";

#define NVS_NAMESPACE "sipwire"
#define POST_TIMEOUT pdMS_TO_TICKS(100)
#define CODEC_PREF_DEFAULT "g722,pcmu,pcma"

/* One NVS key per settings_t field (max 15 chars). A missing key means "use the
 * default", so fields added later never invalidate data written by old firmware. */
#define K_WIFI_SSID   "wifi_ssid"
#define K_WIFI_PASS   "wifi_pass"
#define K_SIP_SERVER  "sip_server"
#define K_SIP_PORT    "sip_port"
#define K_SIP_TRANS   "sip_transport"
#define K_SIP_USER    "sip_user"
#define K_SIP_AUTH    "sip_auth_user"
#define K_SIP_PASS    "sip_password"
#define K_SIP_NAME    "sip_disp_name"
#define K_SIP_EXPIRES "sip_reg_exp"
#define K_CODEC_PREF  "codec_pref"
#define K_RING_VOL    "ring_vol"
#define K_CALL_VOL    "call_vol"
#define K_MIC_GAIN    "mic_gain"
#define K_BACKLIGHT   "backlight"
#define K_BL_TIMEOUT  "bl_timeout"
#define K_TC_VALID    "tc_valid"
#define K_TC_X_MIN    "tc_x_min"
#define K_TC_X_MAX    "tc_x_max"
#define K_TC_Y_MIN    "tc_y_min"
#define K_TC_Y_MAX    "tc_y_max"

static SemaphoreHandle_t s_lock;
static settings_t s_cur;

static void settings_defaults(settings_t *s)
{
    memset(s, 0, sizeof(*s));
    s->sip_port = 5060;
    s->sip_transport = SIP_TRANSPORT_UDP;
    s->sip_reg_expires = 300;
    strcpy(s->codec_pref, CODEC_PREF_DEFAULT);
    s->ring_volume = 70;
    s->call_volume = 60;
    s->mic_gain = 50;
    s->backlight = 80;
    s->backlight_timeout_s = 30;
}

/* ---- NVS load: a missing (or unusable) key leaves the default in place ---- */

static void load_str(nvs_handle_t h, const char *key, char *dst, size_t size)
{
    size_t len = size;
    if (nvs_get_str(h, key, dst, &len) != ESP_OK) {
        dst[0] = '\0';
    }
}

static void load_u8_max100(nvs_handle_t h, const char *key, uint8_t *dst)
{
    uint8_t v;
    if (nvs_get_u8(h, key, &v) == ESP_OK && v <= 100) {
        *dst = v;
    }
}

static void load_all(nvs_handle_t h, settings_t *s)
{
    uint8_t u8;
    uint16_t u16;

    settings_defaults(s);
    load_str(h, K_WIFI_SSID, s->wifi_ssid, sizeof(s->wifi_ssid));
    load_str(h, K_WIFI_PASS, s->wifi_pass, sizeof(s->wifi_pass));
    load_str(h, K_SIP_SERVER, s->sip_server, sizeof(s->sip_server));
    load_str(h, K_SIP_USER, s->sip_user, sizeof(s->sip_user));
    load_str(h, K_SIP_AUTH, s->sip_auth_user, sizeof(s->sip_auth_user));
    load_str(h, K_SIP_PASS, s->sip_password, sizeof(s->sip_password));
    load_str(h, K_SIP_NAME, s->sip_display_name, sizeof(s->sip_display_name));
    size_t len = sizeof(s->codec_pref);
    if (nvs_get_str(h, K_CODEC_PREF, s->codec_pref, &len) != ESP_OK) {
        strcpy(s->codec_pref, CODEC_PREF_DEFAULT);  /* key added after release: keep the default */
    }

    if (nvs_get_u16(h, K_SIP_PORT, &u16) == ESP_OK && u16 != 0) {
        s->sip_port = u16;
    }
    if (nvs_get_u8(h, K_SIP_TRANS, &u8) == ESP_OK && u8 <= SIP_TRANSPORT_TCP) {
        s->sip_transport = (sip_transport_t)u8;
    }
    nvs_get_u32(h, K_SIP_EXPIRES, &s->sip_reg_expires);

    load_u8_max100(h, K_RING_VOL, &s->ring_volume);
    load_u8_max100(h, K_CALL_VOL, &s->call_volume);
    load_u8_max100(h, K_MIC_GAIN, &s->mic_gain);
    load_u8_max100(h, K_BACKLIGHT, &s->backlight);
    nvs_get_u16(h, K_BL_TIMEOUT, &s->backlight_timeout_s);

    if (nvs_get_u8(h, K_TC_VALID, &u8) == ESP_OK) {
        s->touch_cal.valid = u8 != 0;
    }
    nvs_get_i16(h, K_TC_X_MIN, &s->touch_cal.x_min);
    nvs_get_i16(h, K_TC_X_MAX, &s->touch_cal.x_max);
    nvs_get_i16(h, K_TC_Y_MIN, &s->touch_cal.y_min);
    nvs_get_i16(h, K_TC_Y_MAX, &s->touch_cal.y_max);
}

/* ---- validation ---- */

static bool str_ok(const char *s, size_t size)
{
    return memchr(s, '\0', size) != NULL;
}

static esp_err_t validate(const settings_t *s)
{
    ESP_RETURN_ON_FALSE(str_ok(s->wifi_ssid, sizeof(s->wifi_ssid)) &&
                        str_ok(s->wifi_pass, sizeof(s->wifi_pass)) &&
                        str_ok(s->sip_server, sizeof(s->sip_server)) &&
                        str_ok(s->sip_user, sizeof(s->sip_user)) &&
                        str_ok(s->sip_auth_user, sizeof(s->sip_auth_user)) &&
                        str_ok(s->sip_password, sizeof(s->sip_password)) &&
                        str_ok(s->sip_display_name, sizeof(s->sip_display_name)) &&
                        str_ok(s->codec_pref, sizeof(s->codec_pref)),
                        ESP_ERR_INVALID_ARG, TAG, "string not NUL-terminated");
    ESP_RETURN_ON_FALSE(s->sip_port != 0, ESP_ERR_INVALID_ARG, TAG, "sip_port is 0");
    ESP_RETURN_ON_FALSE((unsigned)s->sip_transport <= SIP_TRANSPORT_TCP, ESP_ERR_INVALID_ARG, TAG,
                        "bad sip_transport");
    ESP_RETURN_ON_FALSE(s->ring_volume <= 100 && s->call_volume <= 100 && s->mic_gain <= 100 &&
                        s->backlight <= 100, ESP_ERR_INVALID_ARG, TAG, "value out of range 0-100");
    return ESP_OK;
}

/* ---- NVS store: write only the keys that differ from the in-RAM copy ---- */

#define STORE_STR(key, field)                                                      \
    do {                                                                           \
        if (strcmp(in->field, s_cur.field) != 0) {                                 \
            ESP_RETURN_ON_ERROR(nvs_set_str(h, key, in->field), TAG, key);         \
        }                                                                          \
    } while (0)

#define STORE_NUM(fn, key, field)                                                  \
    do {                                                                           \
        if (in->field != s_cur.field) {                                            \
            ESP_RETURN_ON_ERROR(fn(h, key, in->field), TAG, key);                  \
        }                                                                          \
    } while (0)

static esp_err_t store_changed(nvs_handle_t h, const settings_t *in)
{
    STORE_STR(K_WIFI_SSID, wifi_ssid);
    STORE_STR(K_WIFI_PASS, wifi_pass);
    STORE_STR(K_SIP_SERVER, sip_server);
    STORE_STR(K_SIP_USER, sip_user);
    STORE_STR(K_SIP_AUTH, sip_auth_user);
    STORE_STR(K_SIP_PASS, sip_password);
    STORE_STR(K_SIP_NAME, sip_display_name);
    STORE_STR(K_CODEC_PREF, codec_pref);

    STORE_NUM(nvs_set_u16, K_SIP_PORT, sip_port);
    if (in->sip_transport != s_cur.sip_transport) {
        ESP_RETURN_ON_ERROR(nvs_set_u8(h, K_SIP_TRANS, (uint8_t)in->sip_transport), TAG, K_SIP_TRANS);
    }
    STORE_NUM(nvs_set_u32, K_SIP_EXPIRES, sip_reg_expires);
    STORE_NUM(nvs_set_u8, K_RING_VOL, ring_volume);
    STORE_NUM(nvs_set_u8, K_CALL_VOL, call_volume);
    STORE_NUM(nvs_set_u8, K_MIC_GAIN, mic_gain);
    STORE_NUM(nvs_set_u8, K_BACKLIGHT, backlight);
    STORE_NUM(nvs_set_u16, K_BL_TIMEOUT, backlight_timeout_s);
    if (in->touch_cal.valid != s_cur.touch_cal.valid) {
        ESP_RETURN_ON_ERROR(nvs_set_u8(h, K_TC_VALID, in->touch_cal.valid), TAG, K_TC_VALID);
    }
    STORE_NUM(nvs_set_i16, K_TC_X_MIN, touch_cal.x_min);
    STORE_NUM(nvs_set_i16, K_TC_X_MAX, touch_cal.x_max);
    STORE_NUM(nvs_set_i16, K_TC_Y_MIN, touch_cal.y_min);
    STORE_NUM(nvs_set_i16, K_TC_Y_MAX, touch_cal.y_max);
    return ESP_OK;
}

static void post_changed(void)
{
    esp_err_t err = esp_event_post(SIPWIRE_SETTINGS_EVENT, SETTINGS_EVENT_CHANGED, NULL, 0, POST_TIMEOUT);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "post SETTINGS_EVENT_CHANGED failed: %s", esp_err_to_name(err));
    }
}

/* ---- public API ---- */

esp_err_t settings_init(void)
{
    ESP_RETURN_ON_FALSE(s_lock == NULL, ESP_ERR_INVALID_STATE, TAG, "already initialized");
    SemaphoreHandle_t lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(lock != NULL, ESP_ERR_NO_MEM, TAG, "no mem for mutex");

    settings_defaults(&s_cur);
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_OK) {
        load_all(h, &s_cur);
        nvs_close(h);
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "no stored settings, using defaults");
    } else {
        vSemaphoreDelete(lock);
        ESP_RETURN_ON_ERROR(err, TAG, "nvs_open");
    }
    s_lock = lock;

    /* Never log passwords. */
    ESP_LOGI(TAG, "loaded: ssid='%s' sip=%s@%s:%u", s_cur.wifi_ssid, s_cur.sip_user, s_cur.sip_server,
             s_cur.sip_port);
    return ESP_OK;
}

void settings_get(settings_t *out)
{
    if (s_lock == NULL) {
        settings_defaults(out);
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_cur;
    xSemaphoreGive(s_lock);
}

esp_err_t settings_set(const settings_t *in)
{
    ESP_RETURN_ON_FALSE(in != NULL, ESP_ERR_INVALID_ARG, TAG, "null settings");
    ESP_RETURN_ON_FALSE(s_lock != NULL, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    ESP_RETURN_ON_ERROR(validate(in), TAG, "invalid settings");

    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = ESP_OK;
    bool changed = memcmp(in, &s_cur, sizeof(*in)) != 0;
    if (changed) {
        nvs_handle_t h;
        err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
        if (err == ESP_OK) {
            err = store_changed(h, in);
            if (err == ESP_OK) {
                err = nvs_commit(h);
            }
            if (err == ESP_OK) {
                s_cur = *in;
            } else {
                /* Some keys may already be written: resync RAM with what NVS holds. */
                load_all(h, &s_cur);
            }
            nvs_close(h);
        }
    }
    xSemaphoreGive(s_lock);

    ESP_RETURN_ON_ERROR(err, TAG, "persist failed");
    if (changed) {
        post_changed();
    }
    return ESP_OK;
}

esp_err_t settings_reset(void)
{
    ESP_RETURN_ON_FALSE(s_lock != NULL, ESP_ERR_INVALID_STATE, TAG, "not initialized");

    xSemaphoreTake(s_lock, portMAX_DELAY);
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        /* Erased keys read back as "missing", i.e. defaults. */
        err = nvs_erase_all(h);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (err == ESP_OK) {
        settings_defaults(&s_cur);
    }
    xSemaphoreGive(s_lock);

    ESP_RETURN_ON_ERROR(err, TAG, "reset failed");
    post_changed();
    return ESP_OK;
}
