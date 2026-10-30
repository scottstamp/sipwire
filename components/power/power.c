#include "power.h"

#include <stdio.h>
#include <string.h>
#include "console.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_rom_sys.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"

static const char *TAG = "power";

/* DFS needs the IDF power management layer; QEMU does not model the clock switching. */
#if defined(CONFIG_PM_ENABLE) && defined(CONFIG_SIPWIRE_POWER_DFS) && !defined(CONFIG_SIPWIRE_QEMU)
#define POWER_DFS 1
#else
#define POWER_DFS 0
#endif

static const char *const s_hold_names[POWER_HOLD_COUNT] = {
    [POWER_HOLD_CALL] = "call",
    [POWER_HOLD_DISPLAY] = "display",
};

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;   /* guards s_bits before init */
static SemaphoreHandle_t s_lock;     /* after init: serialises bit changes with lock calls */
static unsigned s_bits;
#if POWER_DFS
static bool s_dfs;                   /* esp_pm_configure succeeded */
static esp_pm_lock_handle_t s_pm[POWER_HOLD_COUNT];
#endif

static void apply_lock(power_hold_t who, bool hold)
{
#if POWER_DFS
    if (s_dfs && s_pm[who]) {
        esp_err_t err = hold ? esp_pm_lock_acquire(s_pm[who]) : esp_pm_lock_release(s_pm[who]);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "%s %s: %s", hold ? "acquire" : "release", s_hold_names[who], esp_err_to_name(err));
        }
    }
#else
    (void)who;
    (void)hold;
#endif
}

void power_hold(power_hold_t who, bool hold)
{
    if ((unsigned)who >= POWER_HOLD_COUNT) {
        return;
    }
    unsigned bit = 1u << who;
    if (s_lock == NULL) {
        /* before power_init: remember it, power_init acquires what is set */
        portENTER_CRITICAL(&s_mux);
        s_bits = hold ? (s_bits | bit) : (s_bits & ~bit);
        portEXIT_CRITICAL(&s_mux);
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool was = (s_bits & bit) != 0;
    if (was != hold) {
        portENTER_CRITICAL(&s_mux);
        s_bits = hold ? (s_bits | bit) : (s_bits & ~bit);
        portEXIT_CRITICAL(&s_mux);
        apply_lock(who, hold);
        ESP_LOGD(TAG, "%s hold %s", s_hold_names[who], hold ? "on" : "off");
    }
    xSemaphoreGive(s_lock);
}

unsigned power_holds(void)
{
    portENTER_CRITICAL(&s_mux);
    unsigned b = s_bits;
    portEXIT_CRITICAL(&s_mux);
    return b;
}

/* ------------------------------------------------------------------ console */

static const char *wifi_ps_name(void)
{
#if defined(CONFIG_SIPWIRE_QEMU)
    return "n/a (QEMU: Ethernet)";
#else
    wifi_ps_type_t ps;
    if (esp_wifi_get_ps(&ps) != ESP_OK) {
        return "n/a (wifi not started)";
    }
    switch (ps) {
    case WIFI_PS_NONE:      return "off (WIFI_PS_NONE, call)";
    case WIFI_PS_MIN_MODEM: return "modem sleep (WIFI_PS_MIN_MODEM, wakes every DTIM)";
    case WIFI_PS_MAX_MODEM: return "modem sleep (WIFI_PS_MAX_MODEM)";
    default:                return "?";
    }
#endif
}

static int cmd_power(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "locks") == 0) {
#if defined(CONFIG_PM_ENABLE)
        esp_pm_dump_locks(stdout);
#else
        printf("CONFIG_PM_ENABLE is off: no PM locks\n");
#endif
        return 0;
    }
    if (argc >= 2) {
        printf("usage: power [locks]\n");
        return 1;
    }

    unsigned bits = power_holds();
    printf("cpu       : %u MHz now\n", (unsigned)esp_rom_get_cpu_ticks_per_us());
#if POWER_DFS
    esp_pm_config_t pc = { 0 };
    if (s_dfs && esp_pm_get_configuration(&pc) == ESP_OK) {
        printf("dfs       : on, %d..%d MHz (max while held), light sleep %s\n", pc.min_freq_mhz,
               pc.max_freq_mhz, pc.light_sleep_enable ? "on" : "off");
    } else {
        printf("dfs       : configure failed, fixed %d MHz\n", CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
    }
#elif defined(CONFIG_SIPWIRE_QEMU)
    printf("dfs       : off (QEMU build)\n");
#else
    printf("dfs       : off (CONFIG_PM_ENABLE / CONFIG_SIPWIRE_POWER_DFS), fixed %d MHz\n",
           CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
#endif
    printf("holds     :");
    if (bits == 0) {
        printf(" none");
#if POWER_DFS
        printf(" (CPU at the DFS minimum)");
#endif
    }
    for (int i = 0; i < POWER_HOLD_COUNT; i++) {
        if (bits & (1u << i)) {
            printf(" %s", s_hold_names[i]);
        }
    }
    printf("\n");
    printf("wifi ps   : %s\n", wifi_ps_name());
    printf("sleep     : no automatic light sleep (docs/ARCHITECTURE.md \"Power\")\n");
    return 0;
}

/* ------------------------------------------------------------------ init */

esp_err_t power_init(void)
{
    ESP_RETURN_ON_FALSE(s_lock == NULL, ESP_ERR_INVALID_STATE, TAG, "already initialized");

#if POWER_DFS
    for (int i = 0; i < POWER_HOLD_COUNT; i++) {
        ESP_RETURN_ON_ERROR(esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, s_hold_names[i], &s_pm[i]), TAG,
                            "pm lock %s", s_hold_names[i]);
    }
    const esp_pm_config_t pc = {
        .max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
        .min_freq_mhz = CONFIG_SIPWIRE_POWER_MIN_CPU_MHZ,
        .light_sleep_enable = false,
    };
    esp_err_t err = esp_pm_configure(&pc);
    if (err == ESP_OK) {
        s_dfs = true;
        ESP_LOGI(TAG, "dfs %d..%d MHz", pc.min_freq_mhz, pc.max_freq_mhz);
    } else {
        /* not fatal: the CPU just stays at the default frequency */
        ESP_LOGW(TAG, "esp_pm_configure: %s, cpu fixed at %d MHz", esp_err_to_name(err), pc.max_freq_mhz);
    }
#else
    ESP_LOGI(TAG, "dfs off, cpu fixed at %d MHz", CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
#endif

    SemaphoreHandle_t lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(lock != NULL, ESP_ERR_NO_MEM, TAG, "mutex");
    xSemaphoreTake(lock, portMAX_DELAY);
    s_lock = lock;
    unsigned early = power_holds();     /* holds requested before init */
    for (int i = 0; i < POWER_HOLD_COUNT; i++) {
        if (early & (1u << i)) {
            apply_lock((power_hold_t)i, true);
        }
    }
    xSemaphoreGive(lock);

    const esp_console_cmd_t cmd = {
        .command = "power",
        .help = "Power state: CPU frequency, DFS holds, Wi-Fi power save. `power locks`: PM lock table",
        .hint = "[locks]",
        .func = cmd_power,
    };
    ESP_RETURN_ON_ERROR(console_register_command(&cmd), TAG, "register power");
    return ESP_OK;
}
