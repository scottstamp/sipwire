#include "power.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "console.h"
#include "esp_bit_defs.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_rom_sys.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"
#if defined(CONFIG_ESP_CONSOLE_UART)
#include "driver/uart_wakeup.h"
#endif

static const char *TAG = "power";

/* DFS needs the IDF power management layer; QEMU does not model the clock switching. */
#if defined(CONFIG_PM_ENABLE) && defined(CONFIG_SIPWIRE_POWER_DFS) && !defined(CONFIG_SIPWIRE_QEMU)
#define POWER_DFS 1
#else
#define POWER_DFS 0
#endif

/* Automatic light sleep on top of DFS (Kconfig depends on FREERTOS_USE_TICKLESS_IDLE). */
#if POWER_DFS && defined(CONFIG_SIPWIRE_POWER_LIGHT_SLEEP)
#define POWER_LS 1
#else
#define POWER_LS 0
#endif

/* Sleep count / time / wake causes and the console wake-up, from the light-sleep exit callback
 * (SIPWIRE_POWER_LIGHT_SLEEP selects PM_LIGHT_SLEEP_CALLBACKS). */
#if POWER_LS && defined(CONFIG_PM_LIGHT_SLEEP_CALLBACKS)
#define POWER_LS_STATS 1
#else
#define POWER_LS_STATS 0
#endif

#define UART_WAKE_EDGES     3       /* RX edges that wake the chip (those characters are lost) */

static const char *const s_hold_names[POWER_HOLD_COUNT] = {
    [POWER_HOLD_CALL] = "call",
    [POWER_HOLD_DISPLAY] = "display",
    [POWER_HOLD_CONSOLE] = "console",
};

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;   /* guards s_bits before init */
static SemaphoreHandle_t s_lock;     /* after init: serialises bit changes with lock calls */
static unsigned s_bits;
#if POWER_DFS
static bool s_dfs;                   /* esp_pm_configure succeeded */
static bool s_ls;                    /* ... with light_sleep_enable */
static esp_pm_lock_handle_t s_pm[POWER_HOLD_COUNT];     /* CPU_FREQ_MAX (not for console) */
static esp_pm_lock_handle_t s_nls[POWER_HOLD_COUNT];    /* NO_LIGHT_SLEEP */
#endif
#if POWER_LS
static esp_timer_handle_t s_con_kick;   /* UART wake-up seen: take the console hold */
static esp_timer_handle_t s_con_idle;   /* console quiet for CONSOLE_AWAKE_S: drop it */
#endif

#if POWER_DFS
static void lock_op(esp_pm_lock_handle_t h, bool hold, const char *what)
{
    if (h == NULL) {
        return;
    }
    esp_err_t err = hold ? esp_pm_lock_acquire(h) : esp_pm_lock_release(h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s %s: %s", hold ? "acquire" : "release", what, esp_err_to_name(err));
    }
}
#endif

static void apply_lock(power_hold_t who, bool hold)
{
#if POWER_DFS
    if (s_dfs) {
        lock_op(s_pm[who], hold, s_hold_names[who]);
        lock_op(s_nls[who], hold, s_hold_names[who]);
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

/* ------------------------------------------------------------------ console hold */

#if POWER_LS
/* While someone types, light sleep would eat characters (a character arriving in light sleep
 * only wakes the chip). So any console input, and the UART wake-up itself, keeps the chip
 * awake until the console has been quiet for CONFIG_SIPWIRE_POWER_CONSOLE_AWAKE_S. Tasks only
 * (console task, esp_timer task). */
#define CONSOLE_AWAKE_US    ((int64_t)CONFIG_SIPWIRE_POWER_CONSOLE_AWAKE_S * 1000000)

static _Atomic int64_t s_con_last_us;  /* esp_timer time of the last console input */

static void console_activity(void)
{
    s_con_last_us = esp_timer_get_time();
    power_hold(POWER_HOLD_CONSOLE, true);
    esp_timer_start_once(s_con_idle, CONSOLE_AWAKE_US); /* ESP_ERR_INVALID_STATE if armed */
}

static void con_kick_cb(void *arg)
{
    (void)arg;
    console_activity();
}

/* Not esp_timer_is_active() to see a restart: it is true inside the timer's own callback, so
 * the hold was never dropped. The timer only ever runs once per quiet period; input in between
 * just moves s_con_last_us and the callback re-arms for the rest. */
static void con_idle_cb(void *arg)
{
    (void)arg;
    int64_t last = s_con_last_us;
    int64_t quiet = esp_timer_get_time() - last;
    if (quiet < CONSOLE_AWAKE_US) {
        esp_timer_start_once(s_con_idle, CONSOLE_AWAKE_US - quiet);
        return;
    }
    power_hold(POWER_HOLD_CONSOLE, false);
    if (s_con_last_us != last) {                        /* input raced the release */
        console_activity();
    }
}
#endif

/* ------------------------------------------------------------------ light sleep stats */

#if POWER_LS_STATS
typedef enum { WAKE_TIMER, WAKE_GPIO, WAKE_UART, WAKE_WIFI, WAKE_OTHER, WAKE_N } wake_t;
static const char *const s_wake_names[WAKE_N] = { "timer", "gpio", "uart", "wifi", "other" };

typedef struct {
    uint32_t sleeps;
    uint64_t slept_us;
    uint32_t wake[WAKE_N];
} ls_stats_t;

static portMUX_TYPE s_stat_mux = portMUX_INITIALIZER_UNLOCKED;
static ls_stats_t s_stats;

/* IDLE task, inside the PM layer's critical section, after every light-sleep attempt: no
 * blocking calls. esp_timer_start_once is IRAM and safe here. */
static esp_err_t ls_exit_cb(int64_t slept_us, void *arg)
{
    (void)arg;
    if (slept_us <= 0) {
        return ESP_OK;                  /* too short to sleep, or rejected */
    }
    uint32_t causes = esp_sleep_get_wakeup_causes();
    bool uart = causes & (BIT(ESP_SLEEP_WAKEUP_UART0) | BIT(ESP_SLEEP_WAKEUP_UART1));
    wake_t w = WAKE_OTHER;
    if (uart) {
        w = WAKE_UART;
    } else if (causes & BIT(ESP_SLEEP_WAKEUP_GPIO)) {
        w = WAKE_GPIO;
    } else if (causes & BIT(ESP_SLEEP_WAKEUP_WIFI)) {
        w = WAKE_WIFI;
    } else if (causes & BIT(ESP_SLEEP_WAKEUP_TIMER)) {
        w = WAKE_TIMER;
    }
    portENTER_CRITICAL_SAFE(&s_stat_mux);
    s_stats.sleeps++;
    s_stats.slept_us += (uint64_t)slept_us;
    s_stats.wake[w]++;
    portEXIT_CRITICAL_SAFE(&s_stat_mux);
    if (uart) {
        esp_timer_start_once(s_con_kick, 0);    /* ESP_ERR_INVALID_STATE if already pending */
    }
    return ESP_OK;
}

static void ls_stats_get(ls_stats_t *out)
{
    portENTER_CRITICAL(&s_stat_mux);
    *out = s_stats;
    portEXIT_CRITICAL(&s_stat_mux);
}
#endif

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

static void print_sleep(unsigned bits)
{
#if POWER_LS
    if (!s_ls) {
        printf("sleep     : light sleep configured but rejected by esp_pm (see boot log)\n");
        return;
    }
    printf("sleep     : automatic light sleep, %s\n",
           bits ? "blocked now by the holds above" : "whenever both cores idle");
    printf("wake      : wifi (DTIM beacons), keypad + touch (gpio), console uart (first chars "
           "lost, then awake %d s after the last input)\n", CONFIG_SIPWIRE_POWER_CONSOLE_AWAKE_S);
#if POWER_LS_STATS
    static ls_stats_t last;             /* console task only */
    static int64_t last_us;
    ls_stats_t st;
    ls_stats_get(&st);
    int64_t now = esp_timer_get_time();
    printf("asleep    : %" PRIu32 " sleeps, %.1f s = %.1f %% of %.0f s since boot\n", st.sleeps,
           (double)st.slept_us / 1e6, now > 0 ? 100.0 * (double)st.slept_us / (double)now : 0.0,
           (double)now / 1e6);
    if (last_us > 0) {
        double span = (double)(now - last_us);
        double slept = (double)(st.slept_us - last.slept_us);
        printf("            %" PRIu32 " sleeps, %.1f s = %.1f %% of the %.0f s since the last `power`\n",
               st.sleeps - last.sleeps, slept / 1e6, span > 0 ? 100.0 * slept / span : 0.0, span / 1e6);
    }
    printf("woken by  :");
    for (int i = 0; i < WAKE_N; i++) {
        printf(" %s %" PRIu32, s_wake_names[i], st.wake[i]);
    }
    printf(" (since boot)\n");
    last = st;
    last_us = now;
#endif
#else
    (void)bits;
#if defined(CONFIG_SIPWIRE_QEMU)
    printf("sleep     : no light sleep (QEMU build)\n");
#else
    printf("sleep     : no automatic light sleep (CONFIG_SIPWIRE_POWER_LIGHT_SLEEP off)\n");
#endif
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
    /* Always the maximum: IDF holds its "rtos" lock at full speed while any task runs on a
     * core, and this command is running. The minimum applies only while both cores idle;
     * `power locks` shows the time spent at each frequency (CONFIG_PM_PROFILING). */
    printf("cpu       : %u MHz now (this command is running: idle time is at the minimum)\n",
           (unsigned)esp_rom_get_cpu_ticks_per_us());
#if POWER_DFS
    esp_pm_config_t pc = { 0 };
    if (s_dfs && esp_pm_get_configuration(&pc) == ESP_OK) {
        printf("dfs       : on, %d..%d MHz (max while call/display held), light sleep %s\n",
               pc.min_freq_mhz, pc.max_freq_mhz, pc.light_sleep_enable ? "on" : "off");
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
        printf(" (CPU at the DFS minimum whenever idle)");
#endif
    }
    for (int i = 0; i < POWER_HOLD_COUNT; i++) {
        if (bits & (1u << i)) {
            printf(" %s", s_hold_names[i]);
        }
    }
    printf("\n");
    printf("wifi ps   : %s\n", wifi_ps_name());
    print_sleep(bits);
#if POWER_DFS && defined(CONFIG_PM_PROFILING)
    printf("time per frequency since boot, other PM locks: `power locks` (CPU_MAX = 240, APB_MAX = 80)\n");
#endif
    return 0;
}

/* ------------------------------------------------------------------ init */

#if POWER_LS
/* Wake sources and the console hold; the GPIO pins themselves are armed by keypad / display
 * (gpio_wakeup_enable on the keypad columns and the touch PENIRQ). */
static esp_err_t light_sleep_setup(void)
{
    ESP_RETURN_ON_ERROR(esp_sleep_enable_gpio_wakeup(), TAG, "gpio wakeup");
#if defined(CONFIG_ESP_CONSOLE_UART)
    const uart_wakeup_cfg_t uw = {
        .wakeup_mode = UART_WK_MODE_ACTIVE_THRESH,
        .rx_edge_threshold = UART_WAKE_EDGES,
    };
    ESP_RETURN_ON_ERROR(uart_wakeup_setup(CONFIG_ESP_CONSOLE_UART_NUM, &uw), TAG, "uart wake threshold");
    ESP_RETURN_ON_ERROR(esp_sleep_enable_uart_wakeup(CONFIG_ESP_CONSOLE_UART_NUM), TAG, "uart wakeup");
#endif
    const esp_timer_create_args_t kick = { .callback = con_kick_cb, .name = "con_kick" };
    const esp_timer_create_args_t idle = { .callback = con_idle_cb, .name = "con_idle" };
    ESP_RETURN_ON_ERROR(esp_timer_create(&kick, &s_con_kick), TAG, "timer");
    ESP_RETURN_ON_ERROR(esp_timer_create(&idle, &s_con_idle), TAG, "timer");
    console_set_input_hook(console_activity);
#if POWER_LS_STATS
    esp_pm_sleep_cbs_register_config_t cbs = {
        .exit_cb = ls_exit_cb,
    };
    ESP_RETURN_ON_ERROR(esp_pm_light_sleep_register_cbs(&cbs), TAG, "sleep callbacks");
#endif
    return ESP_OK;
}
#endif

esp_err_t power_init(void)
{
    ESP_RETURN_ON_FALSE(s_lock == NULL, ESP_ERR_INVALID_STATE, TAG, "already initialized");

#if POWER_DFS
    for (int i = 0; i < POWER_HOLD_COUNT; i++) {
        if (i != POWER_HOLD_CONSOLE) {
            ESP_RETURN_ON_ERROR(esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, s_hold_names[i], &s_pm[i]), TAG,
                                "pm lock %s", s_hold_names[i]);
        }
#if POWER_LS
        ESP_RETURN_ON_ERROR(esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, s_hold_names[i], &s_nls[i]), TAG,
                            "pm lock %s", s_hold_names[i]);
#endif
    }
#if POWER_LS
    /* before light sleep can start: the console hold and the wake sources must be in place */
    ESP_RETURN_ON_ERROR(light_sleep_setup(), TAG, "light sleep setup");
#endif
    esp_pm_config_t pc = {
        .max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
        .min_freq_mhz = CONFIG_SIPWIRE_POWER_MIN_CPU_MHZ,
        .light_sleep_enable = POWER_LS,
    };
    esp_err_t err = esp_pm_configure(&pc);
    if (err != ESP_OK && pc.light_sleep_enable) {
        ESP_LOGW(TAG, "esp_pm_configure with light sleep: %s, retrying without", esp_err_to_name(err));
        pc.light_sleep_enable = false;
        err = esp_pm_configure(&pc);
    }
    if (err == ESP_OK) {
        s_dfs = true;
        s_ls = pc.light_sleep_enable;
        ESP_LOGI(TAG, "dfs %d..%d MHz, light sleep %s", pc.min_freq_mhz, pc.max_freq_mhz, s_ls ? "on" : "off");
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
        .help = "Power state: CPU frequency, holds, Wi-Fi power save, light sleep time and wake-ups. "
                "`power locks`: PM lock table",
        .hint = "[locks]",
        .func = cmd_power,
    };
    ESP_RETURN_ON_ERROR(console_register_command(&cmd), TAG, "register power");
    return ESP_OK;
}
