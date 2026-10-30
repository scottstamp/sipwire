#include "settings.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "esp_check.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "sdkconfig.h"
#include "sipwire_events.h"

static const char *TAG = "storage";

#define NVS_NAMESPACE "sipwire"
#define POST_TIMEOUT pdMS_TO_TICKS(100)
#define CODEC_PREF_DEFAULT "g722,pcmu,pcma"
#define VM_NUMBER_DEFAULT "*97"

/* Defaults live in main/Kconfig.projbuild (the fallbacks only matter if it is not part of the build). */
#ifndef CONFIG_SIPWIRE_NTP_SERVER
#define CONFIG_SIPWIRE_NTP_SERVER "pool.ntp.org"
#endif
#ifndef CONFIG_SIPWIRE_TZ
#define CONFIG_SIPWIRE_TZ "UTC0"
#endif
#ifndef CONFIG_SIPWIRE_OTA_URL
#define CONFIG_SIPWIRE_OTA_URL "http://192.168.2.193/ota.bin"
#endif

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
#define K_BLF         "blf"
#define K_VM_NUMBER   "vm_number"
/* call features */
#define K_CALL_WAIT   "call_waiting"
#define K_DND         "dnd"
#define K_AUTO_ANSWER "auto_answer"
#define K_FIND_ALERT  "find_alert"
#define K_QUIET       "quiet_hours"
#define K_QUIET_START "quiet_start"
#define K_QUIET_END   "quiet_end"
#define K_QUIET_CONT  "quiet_contacts"
#define K_FWD_ALWAYS  "fwd_always"
#define K_FWD_BUSY    "fwd_busy"
#define K_FWD_NOANS   "fwd_noans"
#define K_FWD_NOANS_S "fwd_noans_s"
#define K_RING_VOL    "ring_vol"
#define K_CALL_VOL    "call_vol"
#define K_MIC_GAIN    "mic_gain"
#define K_AEC         "aec"
#define K_AEC_SUPP    "aec_supp"
#define K_AEC_SUPP_A  "aec_supp_act"
#define K_BACKLIGHT   "backlight"
#define K_BL_TIMEOUT  "bl_timeout"
#define K_NTP_SERVER  "ntp_server"
#define K_TZ          "tz"
#define K_CLOCK_24H   "clock_24h"
#define K_OTA_URL     "ota_url"
#define K_RING_TONE   "ring_tone"
#define K_SPK_VOL     "spk_vol"
#define K_TC_VALID    "tc_valid"
#define K_TC_X_MIN    "tc_x_min"
#define K_TC_X_MAX    "tc_x_max"
#define K_TC_Y_MIN    "tc_y_min"
#define K_TC_Y_MAX    "tc_y_max"

/* Defaults (flash); also what settings_get() returns before settings_init(). */
static const settings_t s_defaults = {
    .sip_port = 5060,
    .sip_transport = SIP_TRANSPORT_UDP,
    .sip_reg_expires = 300,
    .codec_pref = CODEC_PREF_DEFAULT,
    .vm_number = VM_NUMBER_DEFAULT,
    /* call features */
    .call_waiting = true,
    .dnd = false,
    .auto_answer = true,
    .find_alert = "find",
    .quiet_hours = false,
    .quiet_start = 22 * 60,
    .quiet_end = 7 * 60,
    .quiet_allow_contacts = true,
    .fwd_noanswer_s = 20,
    .ring_volume = 70,
    .call_volume = 60,
    .mic_gain = 50,
    .aec = false,               /* until verified on the hardware */
    .aec_suppress = 40,         /* SpeexDSP's defaults */
    .aec_suppress_act = 15,
    .backlight = 80,
    .backlight_timeout_s = 30,
    .ntp_server = CONFIG_SIPWIRE_NTP_SERVER,
    .tz = CONFIG_SIPWIRE_TZ,
    .clock_24h = false,
    .ring_tone = 0,
    .speaker_volume = 90,
    .ota_url = CONFIG_SIPWIRE_OTA_URL,
};

static SemaphoreHandle_t s_lock;
/* The current settings, under s_lock. Never touched from an ISR or with the cache disabled
 * (esp_flash copies NVS data to/from PSRAM through an internal bounce buffer), so it lives in
 * PSRAM when there is some: internal RAM is the scarce resource. */
static settings_t *s_cur;

static void settings_defaults(settings_t *s)
{
    memcpy(s, &s_defaults, sizeof(*s));
}

/* Cold settings_t-sized buffers: PSRAM first (under the 16 KB SPIRAM_MALLOC_ALWAYSINTERNAL
 * threshold plain malloc would take internal RAM). */
static settings_t *alloc_settings(void)
{
    return heap_caps_malloc_prefer(sizeof(settings_t), 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_8BIT);
}

/* ---- value rules ---- */

/* SIP token characters (RFC 3261 §25.1): the alert marker must be one. */
static bool token_ok(const char *s)
{
    for (; *s; s++) {
        if (!((*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') ||
              strchr("-.!%*_+`'~", *s))) {
            return false;
        }
    }
    return true;
}

bool settings_fwd_target_ok(const char *s)
{
    if (s == NULL) {
        return false;
    }
    if (strncmp(s, "sip:", 4) == 0) {
        if (s[4] == '\0') {
            return false;
        }
        for (const char *p = s + 4; *p; p++) {
            if (*p <= ' ' || *p >= 0x7f || strchr("<>\"", *p)) {
                return false;
            }
        }
        return true;
    }
    for (; *s; s++) {
        if (!sipwire_blf_ext_char_ok(*s)) {
            return false;
        }
    }
    return true;
}

/* ---- NVS load: a missing (or unusable) key leaves the default in place ---- */

static void load_str(nvs_handle_t h, const char *key, char *dst, size_t size)
{
    size_t len = size;
    if (nvs_get_str(h, key, dst, &len) != ESP_OK) {
        dst[0] = '\0';
    }
}

/* Like load_str, but a missing key keeps the default already in `dst`
 * (nvs_get_str leaves the buffer alone when it fails). */
static void load_str_or_keep(nvs_handle_t h, const char *key, char *dst, size_t size)
{
    size_t len = size;
    (void)nvs_get_str(h, key, dst, &len);
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

    load_str_or_keep(h, K_BLF, s->blf, sizeof(s->blf));
    load_str_or_keep(h, K_VM_NUMBER, s->vm_number, sizeof(s->vm_number));
    if (sipwire_blf_parse(s->blf, NULL, SIPWIRE_BLF_MAX) < 0) {
        ESP_LOGW(TAG, "stored blf list invalid, ignored");
        s->blf[0] = '\0';
    }
    load_str_or_keep(h, K_NTP_SERVER, s->ntp_server, sizeof(s->ntp_server));
    load_str_or_keep(h, K_TZ, s->tz, sizeof(s->tz));
    load_str_or_keep(h, K_OTA_URL, s->ota_url, sizeof(s->ota_url));
    if (s->ntp_server[0] == '\0') {
        strcpy(s->ntp_server, CONFIG_SIPWIRE_NTP_SERVER);
    }
    if (s->tz[0] == '\0') {
        strcpy(s->tz, CONFIG_SIPWIRE_TZ);
    }
    if (nvs_get_u8(h, K_CLOCK_24H, &u8) == ESP_OK) {
        s->clock_24h = u8 != 0;
    }
    /* call features */
    if (nvs_get_u8(h, K_CALL_WAIT, &u8) == ESP_OK) {
        s->call_waiting = u8 != 0;
    }
    if (nvs_get_u8(h, K_DND, &u8) == ESP_OK) {
        s->dnd = u8 != 0;
    }
    if (nvs_get_u8(h, K_AUTO_ANSWER, &u8) == ESP_OK) {
        s->auto_answer = u8 != 0;
    }
    load_str_or_keep(h, K_FIND_ALERT, s->find_alert, sizeof(s->find_alert));
    if (!token_ok(s->find_alert)) {
        strcpy(s->find_alert, s_defaults.find_alert);
    }
    if (nvs_get_u8(h, K_QUIET, &u8) == ESP_OK) {
        s->quiet_hours = u8 != 0;
    }
    if (nvs_get_u16(h, K_QUIET_START, &u16) == ESP_OK && u16 < SETTINGS_MINUTES_PER_DAY) {
        s->quiet_start = u16;
    }
    if (nvs_get_u16(h, K_QUIET_END, &u16) == ESP_OK && u16 < SETTINGS_MINUTES_PER_DAY) {
        s->quiet_end = u16;
    }
    if (nvs_get_u8(h, K_QUIET_CONT, &u8) == ESP_OK) {
        s->quiet_allow_contacts = u8 != 0;
    }
    load_str_or_keep(h, K_FWD_ALWAYS, s->fwd_always, sizeof(s->fwd_always));
    load_str_or_keep(h, K_FWD_BUSY, s->fwd_busy, sizeof(s->fwd_busy));
    load_str_or_keep(h, K_FWD_NOANS, s->fwd_noanswer, sizeof(s->fwd_noanswer));
    if (!settings_fwd_target_ok(s->fwd_always)) {
        s->fwd_always[0] = '\0';
    }
    if (!settings_fwd_target_ok(s->fwd_busy)) {
        s->fwd_busy[0] = '\0';
    }
    if (!settings_fwd_target_ok(s->fwd_noanswer)) {
        s->fwd_noanswer[0] = '\0';
    }
    if (nvs_get_u16(h, K_FWD_NOANS_S, &u16) == ESP_OK && u16 >= SETTINGS_FWD_NOANSWER_MIN &&
        u16 <= SETTINGS_FWD_NOANSWER_MAX) {
        s->fwd_noanswer_s = u16;
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
    load_u8_max100(h, K_SPK_VOL, &s->speaker_volume);
    nvs_get_u8(h, K_RING_TONE, &s->ring_tone);
    if (nvs_get_u8(h, K_AEC, &u8) == ESP_OK) {
        s->aec = u8 != 0;
    }
    if (nvs_get_u8(h, K_AEC_SUPP, &u8) == ESP_OK && u8 <= SETTINGS_AEC_SUPPRESS_MAX) {
        s->aec_suppress = u8;
    }
    if (nvs_get_u8(h, K_AEC_SUPP_A, &u8) == ESP_OK && u8 <= SETTINGS_AEC_SUPPRESS_MAX) {
        s->aec_suppress_act = u8;
    }
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
                        str_ok(s->codec_pref, sizeof(s->codec_pref)) &&
                        str_ok(s->blf, sizeof(s->blf)) &&
                        str_ok(s->vm_number, sizeof(s->vm_number)) &&
                        str_ok(s->ntp_server, sizeof(s->ntp_server)) &&
                        str_ok(s->tz, sizeof(s->tz)) &&
                        str_ok(s->ota_url, sizeof(s->ota_url)) &&
                        str_ok(s->find_alert, sizeof(s->find_alert)) &&
                        str_ok(s->fwd_always, sizeof(s->fwd_always)) &&
                        str_ok(s->fwd_busy, sizeof(s->fwd_busy)) &&
                        str_ok(s->fwd_noanswer, sizeof(s->fwd_noanswer)),
                        ESP_ERR_INVALID_ARG, TAG, "string not NUL-terminated");
    ESP_RETURN_ON_FALSE(s->ntp_server[0] != '\0' && s->tz[0] != '\0', ESP_ERR_INVALID_ARG, TAG,
                        "ntp_server and tz must not be empty");
    ESP_RETURN_ON_FALSE(s->sip_port != 0, ESP_ERR_INVALID_ARG, TAG, "sip_port is 0");
    ESP_RETURN_ON_FALSE(sipwire_blf_parse(s->blf, NULL, SIPWIRE_BLF_MAX) >= 0, ESP_ERR_INVALID_ARG, TAG,
                        "blf: up to %d entries ext[:label], comma separated, no repeats", SIPWIRE_BLF_MAX);
    ESP_RETURN_ON_FALSE((unsigned)s->sip_transport <= SIP_TRANSPORT_TCP, ESP_ERR_INVALID_ARG, TAG,
                        "bad sip_transport");
    ESP_RETURN_ON_FALSE(s->ring_volume <= 100 && s->call_volume <= 100 && s->speaker_volume <= 100 && s->mic_gain <= 100 &&
                        s->backlight <= 100, ESP_ERR_INVALID_ARG, TAG, "value out of range 0-100");
    ESP_RETURN_ON_FALSE(token_ok(s->find_alert), ESP_ERR_INVALID_ARG, TAG,
                        "find_alert: letters, digits and - . ! %% * _ + ` ' ~ only");
    ESP_RETURN_ON_FALSE(s->quiet_start < SETTINGS_MINUTES_PER_DAY && s->quiet_end < SETTINGS_MINUTES_PER_DAY,
                        ESP_ERR_INVALID_ARG, TAG, "quiet_start / quiet_end: 00:00-23:59");
    ESP_RETURN_ON_FALSE(settings_fwd_target_ok(s->fwd_always) && settings_fwd_target_ok(s->fwd_busy) &&
                        settings_fwd_target_ok(s->fwd_noanswer), ESP_ERR_INVALID_ARG, TAG,
                        "forward target: a number (0-9 a-z * # + . _ -) or a sip: URI");
    ESP_RETURN_ON_FALSE(s->fwd_noanswer_s >= SETTINGS_FWD_NOANSWER_MIN && s->fwd_noanswer_s <= SETTINGS_FWD_NOANSWER_MAX,
                        ESP_ERR_INVALID_ARG, TAG, "fwd_noanswer_s: %d-%d", SETTINGS_FWD_NOANSWER_MIN,
                        SETTINGS_FWD_NOANSWER_MAX);
    ESP_RETURN_ON_FALSE(s->aec_suppress <= SETTINGS_AEC_SUPPRESS_MAX && s->aec_suppress_act <= SETTINGS_AEC_SUPPRESS_MAX,
                        ESP_ERR_INVALID_ARG, TAG, "aec_suppress: 0-%d dB", SETTINGS_AEC_SUPPRESS_MAX);
    return ESP_OK;
}

/* ---- NVS store: write only the keys that differ from the in-RAM copy ---- */

#define STORE_STR(key, field)                                                      \
    do {                                                                           \
        if (strcmp(in->field, s_cur->field) != 0) {                                \
            ESP_RETURN_ON_ERROR(nvs_set_str(h, key, in->field), TAG, key);         \
        }                                                                          \
    } while (0)

#define STORE_NUM(fn, key, field)                                                  \
    do {                                                                           \
        if (in->field != s_cur->field) {                                           \
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
    STORE_STR(K_BLF, blf);
    STORE_STR(K_VM_NUMBER, vm_number);
    STORE_STR(K_NTP_SERVER, ntp_server);
    STORE_STR(K_TZ, tz);
    STORE_STR(K_OTA_URL, ota_url);
    if (in->clock_24h != s_cur->clock_24h) {
        ESP_RETURN_ON_ERROR(nvs_set_u8(h, K_CLOCK_24H, in->clock_24h ? 1 : 0), TAG, K_CLOCK_24H);
    }
    /* call features */
    if (in->call_waiting != s_cur->call_waiting) {
        ESP_RETURN_ON_ERROR(nvs_set_u8(h, K_CALL_WAIT, in->call_waiting ? 1 : 0), TAG, K_CALL_WAIT);
    }
    if (in->dnd != s_cur->dnd) {
        ESP_RETURN_ON_ERROR(nvs_set_u8(h, K_DND, in->dnd ? 1 : 0), TAG, K_DND);
    }
    if (in->auto_answer != s_cur->auto_answer) {
        ESP_RETURN_ON_ERROR(nvs_set_u8(h, K_AUTO_ANSWER, in->auto_answer ? 1 : 0), TAG, K_AUTO_ANSWER);
    }
    STORE_STR(K_FIND_ALERT, find_alert);
    if (in->quiet_hours != s_cur->quiet_hours) {
        ESP_RETURN_ON_ERROR(nvs_set_u8(h, K_QUIET, in->quiet_hours ? 1 : 0), TAG, K_QUIET);
    }
    STORE_NUM(nvs_set_u16, K_QUIET_START, quiet_start);
    STORE_NUM(nvs_set_u16, K_QUIET_END, quiet_end);
    if (in->quiet_allow_contacts != s_cur->quiet_allow_contacts) {
        ESP_RETURN_ON_ERROR(nvs_set_u8(h, K_QUIET_CONT, in->quiet_allow_contacts ? 1 : 0), TAG, K_QUIET_CONT);
    }
    STORE_STR(K_FWD_ALWAYS, fwd_always);
    STORE_STR(K_FWD_BUSY, fwd_busy);
    STORE_STR(K_FWD_NOANS, fwd_noanswer);
    STORE_NUM(nvs_set_u16, K_FWD_NOANS_S, fwd_noanswer_s);

    STORE_NUM(nvs_set_u16, K_SIP_PORT, sip_port);
    if (in->sip_transport != s_cur->sip_transport) {
        ESP_RETURN_ON_ERROR(nvs_set_u8(h, K_SIP_TRANS, (uint8_t)in->sip_transport), TAG, K_SIP_TRANS);
    }
    STORE_NUM(nvs_set_u32, K_SIP_EXPIRES, sip_reg_expires);
    STORE_NUM(nvs_set_u8, K_RING_VOL, ring_volume);
    STORE_NUM(nvs_set_u8, K_CALL_VOL, call_volume);
    STORE_NUM(nvs_set_u8, K_MIC_GAIN, mic_gain);
    STORE_NUM(nvs_set_u8, K_SPK_VOL, speaker_volume);
    STORE_NUM(nvs_set_u8, K_RING_TONE, ring_tone);
    if (in->aec != s_cur->aec) {
        ESP_RETURN_ON_ERROR(nvs_set_u8(h, K_AEC, in->aec ? 1 : 0), TAG, K_AEC);
    }
    STORE_NUM(nvs_set_u8, K_AEC_SUPP, aec_suppress);
    STORE_NUM(nvs_set_u8, K_AEC_SUPP_A, aec_suppress_act);
    STORE_NUM(nvs_set_u8, K_BACKLIGHT, backlight);
    STORE_NUM(nvs_set_u16, K_BL_TIMEOUT, backlight_timeout_s);
    if (in->touch_cal.valid != s_cur->touch_cal.valid) {
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
    settings_t *cur = alloc_settings();
    SemaphoreHandle_t lock = xSemaphoreCreateMutex();
    if (cur == NULL || lock == NULL) {
        free(cur);
        if (lock) {
            vSemaphoreDelete(lock);
        }
        ESP_LOGE(TAG, "no mem for settings");
        return ESP_ERR_NO_MEM;
    }

    settings_defaults(cur);
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_OK) {
        load_all(h, cur);
        nvs_close(h);
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "no stored settings, using defaults");
    } else {
        free(cur);
        vSemaphoreDelete(lock);
        ESP_RETURN_ON_ERROR(err, TAG, "nvs_open");
    }
    s_cur = cur;
    s_lock = lock;

    /* Never log passwords. */
    ESP_LOGI(TAG, "loaded: ssid='%s' sip=%s@%s:%u", s_cur->wifi_ssid, s_cur->sip_user, s_cur->sip_server,
             s_cur->sip_port);
    return ESP_OK;
}

void settings_get(settings_t *out)
{
    settings_get_field(0, out, sizeof(*out));
}

void settings_get_field(size_t off, void *dst, size_t len)
{
    assert(off <= sizeof(settings_t) && len <= sizeof(settings_t) - off);
    if (s_lock == NULL) {
        memcpy(dst, (const char *)&s_defaults + off, len);
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(dst, (const char *)s_cur + off, len);
    xSemaphoreGive(s_lock);
}

/* Validate and persist `in` as the new settings; caller holds s_lock. *changed: it differed. */
static esp_err_t set_locked(const settings_t *in, bool *changed)
{
    ESP_RETURN_ON_ERROR(validate(in), TAG, "invalid settings");
    esp_err_t err = ESP_OK;
    *changed = memcmp(in, s_cur, sizeof(*in)) != 0;
    if (*changed) {
        nvs_handle_t h;
        err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
        if (err == ESP_OK) {
            err = store_changed(h, in);
            if (err == ESP_OK) {
                err = nvs_commit(h);
            }
            if (err == ESP_OK) {
                memcpy(s_cur, in, sizeof(*s_cur));
            } else {
                /* Some keys may already be written: resync RAM with what NVS holds. */
                load_all(h, s_cur);
            }
            nvs_close(h);
        }
    }
    ESP_RETURN_ON_ERROR(err, TAG, "persist failed");
    return ESP_OK;
}

esp_err_t settings_set(const settings_t *in)
{
    ESP_RETURN_ON_FALSE(in != NULL, ESP_ERR_INVALID_ARG, TAG, "null settings");
    ESP_RETURN_ON_FALSE(s_lock != NULL, ESP_ERR_INVALID_STATE, TAG, "not initialized");

    bool changed = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = set_locked(in, &changed);
    xSemaphoreGive(s_lock);

    if (err == ESP_OK && changed) {
        post_changed();
    }
    return err;
}

esp_err_t settings_set_field(size_t off, const void *src, size_t len)
{
    ESP_RETURN_ON_FALSE(src != NULL && off <= sizeof(settings_t) && len <= sizeof(settings_t) - off,
                        ESP_ERR_INVALID_ARG, TAG, "bad field");
    ESP_RETURN_ON_FALSE(s_lock != NULL, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    settings_t *next = alloc_settings();
    ESP_RETURN_ON_FALSE(next != NULL, ESP_ERR_NO_MEM, TAG, "no mem");

    bool changed = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(next, s_cur, sizeof(*next));
    memcpy((char *)next + off, src, len);
    esp_err_t err = set_locked(next, &changed);
    xSemaphoreGive(s_lock);
    free(next);

    if (err == ESP_OK && changed) {
        post_changed();
    }
    return err;
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
        settings_defaults(s_cur);
    }
    xSemaphoreGive(s_lock);

    ESP_RETURN_ON_ERROR(err, TAG, "reset failed");
    post_changed();
    return ESP_OK;
}
