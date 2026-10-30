#include "console.h"

#include <errno.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/select.h>
#include <unistd.h>
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "linenoise/linenoise.h"
#include "net.h"
#include "sdkconfig.h"
#include "settings.h"

/* Command output goes straight to the console with printf (it is the user's reply, not a log). */

static const char *TAG = "console";

#define PROMPT "sipwire> "

/* ---- cfg: table of settings_t fields, addressed by field name ---- */

typedef enum {
    F_STR,
    F_SECRET,       /* string, masked in `cfg show` */
    F_U8,
    F_U16,
    F_U32,
    F_I16,
    F_BOOL,
    F_TRANSPORT,
    F_HHMM,         /* uint16_t minutes after midnight, shown and set as "HH:MM" (or minutes) */
} field_type_t;

typedef struct {
    const char *name;
    field_type_t type;
    size_t offset;
    size_t size;            /* F_STR / F_SECRET: buffer size */
    uint32_t max;           /* numeric types: upper bound */
} field_t;

#define FIELD(n, t, sz, mx) { #n, t, offsetof(settings_t, n), sz, mx }
#define STR_FIELD(n, t) FIELD(n, t, sizeof(((settings_t *)0)->n), 0)

static const field_t s_fields[] = {
    STR_FIELD(wifi_ssid, F_STR),
    STR_FIELD(wifi_pass, F_SECRET),
    STR_FIELD(sip_server, F_STR),
    FIELD(sip_port, F_U16, 0, UINT16_MAX),
    FIELD(sip_transport, F_TRANSPORT, 0, 0),
    STR_FIELD(sip_user, F_STR),
    STR_FIELD(sip_auth_user, F_STR),
    STR_FIELD(sip_password, F_SECRET),
    STR_FIELD(sip_display_name, F_STR),
    FIELD(sip_reg_expires, F_U32, 0, UINT32_MAX),
    STR_FIELD(codec_pref, F_STR),
    STR_FIELD(blf, F_STR),
    STR_FIELD(vm_number, F_STR),
    /* call features */
    FIELD(call_waiting, F_BOOL, 0, 1),
    FIELD(dnd, F_BOOL, 0, 1),
    FIELD(auto_answer, F_BOOL, 0, 1),
    FIELD(keypad_lock, F_BOOL, 0, 1),
    STR_FIELD(find_alert, F_STR),
    FIELD(quiet_hours, F_BOOL, 0, 1),
    FIELD(quiet_start, F_HHMM, 0, SETTINGS_MINUTES_PER_DAY - 1),
    FIELD(quiet_end, F_HHMM, 0, SETTINGS_MINUTES_PER_DAY - 1),
    FIELD(quiet_allow_contacts, F_BOOL, 0, 1),
    STR_FIELD(fwd_always, F_STR),
    STR_FIELD(fwd_busy, F_STR),
    STR_FIELD(fwd_noanswer, F_STR),
    FIELD(fwd_noanswer_s, F_U16, 0, SETTINGS_FWD_NOANSWER_MAX),
    FIELD(ring_volume, F_U8, 0, 100),
    FIELD(call_volume, F_U8, 0, 100),
    FIELD(mic_gain, F_U8, 0, 100),
    FIELD(speaker_volume, F_U8, 0, 100),
    FIELD(ring_tone, F_U8, 0, 31),
    FIELD(aec, F_BOOL, 0, 1),
    FIELD(aec_suppress, F_U8, 0, SETTINGS_AEC_SUPPRESS_MAX),
    FIELD(aec_suppress_act, F_U8, 0, SETTINGS_AEC_SUPPRESS_MAX),
    FIELD(backlight, F_U8, 0, 100),
    FIELD(backlight_timeout_s, F_U16, 0, UINT16_MAX),
    STR_FIELD(ntp_server, F_STR),
    STR_FIELD(tz, F_STR),
    FIELD(clock_24h, F_BOOL, 0, 1),
    STR_FIELD(ota_url, F_STR),
    { "touch_cal_valid", F_BOOL, offsetof(settings_t, touch_cal.valid), 0, 1 },
    { "touch_cal_x_min", F_I16, offsetof(settings_t, touch_cal.x_min), 0, 0 },
    { "touch_cal_x_max", F_I16, offsetof(settings_t, touch_cal.x_max), 0, 0 },
    { "touch_cal_y_min", F_I16, offsetof(settings_t, touch_cal.y_min), 0, 0 },
    { "touch_cal_y_max", F_I16, offsetof(settings_t, touch_cal.y_max), 0, 0 },
};

#define NUM_FIELDS (sizeof(s_fields) / sizeof(s_fields[0]))

static void field_print(const field_t *f, const settings_t *s)
{
    const void *p = (const char *)s + f->offset;
    printf("%-20s = ", f->name);
    switch (f->type) {
    case F_STR:
        printf("%s\n", (const char *)p);
        break;
    case F_SECRET:
        printf("%s\n", *(const char *)p ? "****" : "(unset)");
        break;
    case F_U8:
        printf("%u\n", *(const uint8_t *)p);
        break;
    case F_U16:
        printf("%u\n", *(const uint16_t *)p);
        break;
    case F_U32:
        printf("%" PRIu32 "\n", *(const uint32_t *)p);
        break;
    case F_I16:
        printf("%d\n", *(const int16_t *)p);
        break;
    case F_BOOL:
        printf("%s\n", *(const bool *)p ? "true" : "false");
        break;
    case F_TRANSPORT:
        printf("%s\n", *(const sip_transport_t *)p == SIP_TRANSPORT_TCP ? "tcp" : "udp");
        break;
    case F_HHMM: {
        unsigned m = *(const uint16_t *)p;
        printf("%02u:%02u\n", m / 60, m % 60);
        break;
    }
    }
}

/* Parse `val` into the field of `s`. Returns false (with a message) on bad input. */
static bool field_parse(const field_t *f, settings_t *s, const char *val)
{
    void *p = (char *)s + f->offset;
    char *end = NULL;

    if (f->type == F_STR || f->type == F_SECRET) {
        if (strlen(val) >= f->size) {
            printf("%s: too long (max %u chars)\n", f->name, (unsigned)(f->size - 1));
            return false;
        }
        memcpy(p, val, strlen(val) + 1);
        return true;
    }
    if (f->type == F_TRANSPORT) {
        if (strcasecmp(val, "udp") == 0) {
            *(sip_transport_t *)p = SIP_TRANSPORT_UDP;
        } else if (strcasecmp(val, "tcp") == 0) {
            *(sip_transport_t *)p = SIP_TRANSPORT_TCP;
        } else {
            printf("%s: expected udp or tcp\n", f->name);
            return false;
        }
        return true;
    }
    if (f->type == F_BOOL) {
        if (strcmp(val, "1") == 0 || strcasecmp(val, "true") == 0) {
            *(bool *)p = true;
        } else if (strcmp(val, "0") == 0 || strcasecmp(val, "false") == 0) {
            *(bool *)p = false;
        } else {
            printf("%s: expected true or false\n", f->name);
            return false;
        }
        return true;
    }

    if (f->type == F_HHMM) {
        /* "HH:MM" (24 h) or plain minutes after midnight */
        unsigned long h = strtoul(val, &end, 10), m = 0;
        bool ok = val[0] >= '0' && val[0] <= '9' && end != val;
        if (ok && *end == ':') {
            const char *mm = end + 1;
            m = strtoul(mm, &end, 10);
            ok = end - mm == 2 && h < 24 && m < 60;
            h = h * 60 + m;
        }
        if (!ok || *end != '\0' || h > f->max) {
            printf("%s: expected HH:MM (00:00-23:59) or minutes 0..%" PRIu32 "\n", f->name, f->max);
            return false;
        }
        *(uint16_t *)p = (uint16_t)h;
        return true;
    }

    if (f->type == F_I16) {
        long v = strtol(val, &end, 0);
        if (val[0] == '\0' || *end != '\0' || v < INT16_MIN || v > INT16_MAX) {
            printf("%s: expected integer %d..%d\n", f->name, INT16_MIN, INT16_MAX);
            return false;
        }
        *(int16_t *)p = (int16_t)v;
        return true;
    }

    unsigned long long v = strtoull(val, &end, 0);
    if (val[0] == '\0' || val[0] == '-' || *end != '\0' || v > f->max) {
        printf("%s: expected integer 0..%" PRIu32 "\n", f->name, f->max);
        return false;
    }
    switch (f->type) {
    case F_U8:
        *(uint8_t *)p = (uint8_t)v;
        break;
    case F_U16:
        *(uint16_t *)p = (uint16_t)v;
        break;
    default:
        *(uint32_t *)p = (uint32_t)v;
        break;
    }
    return true;
}

/* A settings_t (~1.3 KB) for a command: off the console stack, in PSRAM when there is some
 * (under the 16 KB SPIRAM_MALLOC_ALWAYSINTERNAL threshold plain malloc would take internal RAM). */
static settings_t *alloc_settings(void)
{
    return heap_caps_malloc_prefer(sizeof(settings_t), 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_8BIT);
}

static int cfg_run(settings_t *s, int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "show") == 0) {
        settings_get(s);
        for (size_t i = 0; i < NUM_FIELDS; i++) {
            field_print(&s_fields[i], s);
        }
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "reset") == 0) {
        esp_err_t err = settings_reset();
        if (err != ESP_OK) {
            printf("reset failed: %s\n", esp_err_to_name(err));
            return 1;
        }
        printf("settings reset to defaults\n");
        return 0;
    }
    if ((argc == 3 || argc == 4) && strcmp(argv[1], "set") == 0) {
        const field_t *f = NULL;
        for (size_t i = 0; i < NUM_FIELDS; i++) {
            if (strcmp(argv[2], s_fields[i].name) == 0) {
                f = &s_fields[i];
                break;
            }
        }
        if (f == NULL) {
            printf("unknown key '%s' (see `cfg show`)\n", argv[2]);
            return 1;
        }
        settings_get(s);
        if (!field_parse(f, s, argc == 4 ? argv[3] : "")) {
            return 1;
        }
        esp_err_t err = settings_set(s);
        if (err != ESP_OK) {
            printf("rejected: %s\n", esp_err_to_name(err));
            return 1;
        }
        settings_get(s);
        field_print(f, s);
        return 0;
    }
    printf("usage: cfg show | cfg set <key> [value] | cfg reset\n");
    return 1;
}

static int cmd_cfg(int argc, char **argv)
{
    settings_t *s = alloc_settings();
    if (s == NULL) {
        printf("out of memory\n");
        return 1;
    }
    int ret = cfg_run(s, argc, argv);
    memset(s, 0, sizeof(*s));       /* passwords */
    free(s);
    return ret;
}

/* ---- other built-ins ---- */

static int cmd_wifi(int argc, char **argv)
{
    if (argc < 2 || argc > 3) {
        printf("usage: wifi <ssid> [pass]\n");
        return 1;
    }
    if (strlen(argv[1]) >= SETTINGS_FIELD_SIZE(wifi_ssid) ||
        (argc == 3 && strlen(argv[2]) >= SETTINGS_FIELD_SIZE(wifi_pass))) {
        printf("ssid max %u chars, password max %u chars\n", (unsigned)SETTINGS_FIELD_SIZE(wifi_ssid) - 1,
               (unsigned)SETTINGS_FIELD_SIZE(wifi_pass) - 1);
        return 1;
    }
    /* ssid and password in one settings_set(): net applies them together */
    settings_t *s = alloc_settings();
    if (s == NULL) {
        printf("out of memory\n");
        return 1;
    }
    settings_get(s);
    strcpy(s->wifi_ssid, argv[1]);
    strcpy(s->wifi_pass, argc == 3 ? argv[2] : "");
    esp_err_t err = settings_set(s);
    memset(s, 0, sizeof(*s));       /* passwords */
    free(s);
    if (err != ESP_OK) {
        printf("rejected: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("saved ssid '%s'%s, connecting\n", argv[1], argc == 3 ? "" : " (open network)");
    return 0;
}

static int cmd_ip(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    const char *ip = net_ip_str();
    printf("%s %s\n", net_is_connected() ? "connected" : "disconnected", ip[0] ? ip : "(no ip)");
    return 0;
}

static int cmd_free(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    printf("internal: free %u, min free %u, largest block %u\n",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    if (heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0) {
        printf("psram:    free %u, min free %u, largest block %u\n",
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
               (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM),
               (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
    } else {
        printf("psram:    none\n");
    }
    return 0;
}

#if CONFIG_FREERTOS_USE_TRACE_FACILITY && CONFIG_FREERTOS_USE_STATS_FORMATTING_FUNCTIONS
static int cmd_tasks(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    char *buf = malloc(uxTaskGetNumberOfTasks() * 64 + 128);
    if (buf == NULL) {
        printf("out of memory\n");
        return 1;
    }
#if CONFIG_FREERTOS_VTASKLIST_INCLUDE_COREID
    printf("name            state prio stack-hwm num core(-1 = any)\n");
#else
    printf("name            state prio stack-hwm num\n");
#endif
    vTaskList(buf);
    printf("%s", buf);
    free(buf);
    return 0;
}
#endif

#if CONFIG_FREERTOS_USE_TRACE_FACILITY && CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
#define CPU_MAX_TASKS 40

/* Two of these (~1.6 KB each) are held for the whole sampling window: PSRAM when there is some. */
static TaskStatus_t *cpu_snapshot(UBaseType_t *n, configRUN_TIME_COUNTER_TYPE *total)
{
    TaskStatus_t *st = heap_caps_calloc_prefer(CPU_MAX_TASKS, sizeof(*st), 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                               MALLOC_CAP_8BIT);
    if (st != NULL) {
        *n = uxTaskGetSystemState(st, CPU_MAX_TASKS, total);
    }
    return st;
}

/* Per-task CPU load over a sampling window, in % of one core (IDLE0/IDLE1
 * show what is left on each core). */
static int cmd_cpu(int argc, char **argv)
{
    int secs = argc > 1 ? atoi(argv[1]) : 2;
    if (secs < 1 || secs > 60) {
        printf("usage: cpu [seconds 1-60]\n");
        return 1;
    }
    UBaseType_t n0 = 0, n1 = 0;
    configRUN_TIME_COUNTER_TYPE t0 = 0, t1 = 0;
    TaskStatus_t *a = cpu_snapshot(&n0, &t0);
    vTaskDelay(pdMS_TO_TICKS(secs * 1000));
    TaskStatus_t *b = cpu_snapshot(&n1, &t1);
    if (a == NULL || b == NULL || t1 == t0) {
        printf("snapshot failed\n");
        free(a);
        free(b);
        return 1;
    }
    printf("name            core  cpu%%\n");
    for (UBaseType_t i = 0; i < n1; i++) {
        configRUN_TIME_COUNTER_TYPE prev = 0;
        for (UBaseType_t j = 0; j < n0; j++) {
            if (a[j].xTaskNumber == b[i].xTaskNumber) {
                prev = a[j].ulRunTimeCounter;
                break;
            }
        }
        uint64_t used = (uint64_t)(b[i].ulRunTimeCounter - prev) * 1000 / (t1 - t0);
        BaseType_t c = xTaskGetCoreID(b[i].xHandle);
        int core = c == tskNO_AFFINITY ? -1 : (int)c;
        printf("%-15s %4d %3u.%u\n", b[i].pcTaskName, core, (unsigned)(used / 10), (unsigned)(used % 10));
    }
    free(a);
    free(b);
    return 0;
}
#endif

#if CONFIG_HEAP_POISONING_COMPREHENSIVE || CONFIG_HEAP_POISONING_LIGHT
/* Debug builds (sdkconfig.debug-heap): check the heaps in the background so a corruption is
 * reported close to when it happens, with both cores' backtraces from the abort.
 *
 * heap_caps_check_integrity() walks every block with that heap's lock held, and with
 * HEAP_POISONING_COMPREHENSIVE it also verifies the fill pattern of every free byte. The heap
 * lock is a spinlock (interrupts masked on this core, the other core spins as soon as it
 * allocates): the ~300 KB internal heap takes a few ms, the ~7.5 MB PSRAM heap ~100 ms, which
 * starves the I2S DMA refill (40 ms ring) and the Wi-Fi task -> audible clicks and dropped
 * packets. So the internal heap is checked every interval and PSRAM only every psram_s seconds
 * (default 10, 0 = never). */
static volatile uint32_t s_heapwatch_ms;
static volatile uint32_t s_heapwatch_psram_s = 10;

static void heapwatch_task(void *arg)
{
    (void)arg;
    int64_t last_psram = 0;
    int64_t last_log = 0;
    int64_t psram_us = 0;
    for (;;) {
        uint32_t ms = s_heapwatch_ms;
        if (ms == 0) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }
        int64_t t0 = esp_timer_get_time();
        bool ok = heap_caps_check_integrity(MALLOC_CAP_INTERNAL, true);
        int64_t t1 = esp_timer_get_time();
        uint32_t psram_s = s_heapwatch_psram_s;
        if (ok && psram_s != 0 && t1 - last_psram >= (int64_t)psram_s * 1000000LL) {
            ok = heap_caps_check_integrity(MALLOC_CAP_SPIRAM, true);
            last_psram = esp_timer_get_time();
            psram_us = last_psram - t1;
        }
        if (!ok) {
            ESP_LOGE(TAG, "heapwatch: heap corrupted (details above)");
            abort();
        }
        if (t0 - last_log > 60 * 1000000LL) {
            last_log = t0;
            ESP_LOGI(TAG, "heapwatch: ok, internal check %" PRId64 " ms, last psram check %" PRId64 " ms",
                     (t1 - t0) / 1000, psram_us / 1000);
        }
        vTaskDelay(pdMS_TO_TICKS(ms));
    }
}

static int cmd_heapwatch(int argc, char **argv)
{
    static TaskHandle_t s_task;
    uint32_t ms = argc >= 2 ? (uint32_t)strtoul(argv[1], NULL, 10) : 200;
    uint32_t psram_s = argc >= 3 ? (uint32_t)strtoul(argv[2], NULL, 10) : 10;
    if (s_task == NULL &&
        xTaskCreatePinnedToCore(heapwatch_task, "heapwatch", 3072, NULL, 1, &s_task, 0) != pdPASS) {
        printf("no memory for the task\n");
        return 1;
    }
    s_heapwatch_psram_s = psram_s;
    s_heapwatch_ms = ms;
    xTaskNotifyGive(s_task);
    if (ms == 0) {
        printf("heapwatch: off\n");
    } else if (psram_s == 0) {
        printf("heapwatch: internal every %" PRIu32 " ms, psram never\n", ms);
    } else {
        printf("heapwatch: internal every %" PRIu32 " ms, psram every %" PRIu32 " s (~100 ms with "
               "interrupts masked: expect an audio click)\n", ms, psram_s);
    }
    return 0;
}
#endif

static int cmd_reboot(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    printf("rebooting\n");
    fflush(stdout);
    esp_restart();
    return 0;
}

#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
esp_err_t console_coredump_register(void);      /* console_coredump.c */
#endif

static void (*volatile s_input_hook)(void);

/* linenoise ends the line on any read() <= 0, and the REPL's own reader does a
 * non-blocking read after select(), which can come back EAGAIN: the line got
 * split mid-command and both halves ran. Wait until a byte really arrives. */
static ssize_t console_read(int fd, void *buf, size_t max_bytes)
{
    for (;;) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        if (select(fd + 1, &rfds, NULL, NULL, NULL) < 0 && errno != EINTR) {
            return -1;
        }
        ssize_t n = read(fd, buf, max_bytes);
        if (n > 0 && s_input_hook) {
            s_input_hook();
        }
        if (n > 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
            return n;
        }
    }
}

/* ---- public API ---- */

void console_set_input_hook(void (*hook)(void))
{
    s_input_hook = hook;
}

esp_err_t console_register_command(const esp_console_cmd_t *cmd)
{
    return esp_console_cmd_register(cmd);
}

esp_err_t console_start(void)
{
    static const esp_console_cmd_t cmds[] = {
        { .command = "cfg", .help = "Show/change settings, e.g. `cfg set sip_server 192.168.1.10`, "
                                    "`cfg set blf \"1002:Bob,1003:Front desk\"`",
          .hint = "show | set <key> [value] | reset", .func = cmd_cfg },
        { .command = "wifi", .help = "Save wifi credentials and (re)connect",
          .hint = "<ssid> [pass]", .func = cmd_wifi },
        { .command = "ip", .help = "Show wifi state and IPv4 address", .func = cmd_ip },
        { .command = "free", .help = "Show free heap (internal / PSRAM)", .func = cmd_free },
#if CONFIG_FREERTOS_USE_TRACE_FACILITY && CONFIG_FREERTOS_USE_STATS_FORMATTING_FUNCTIONS
        { .command = "tasks", .help = "List FreeRTOS tasks", .func = cmd_tasks },
#endif
#if CONFIG_FREERTOS_USE_TRACE_FACILITY && CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
        { .command = "cpu", .help = "Per-task CPU load (% of one core) over a sampling window",
          .hint = "[seconds]", .func = cmd_cpu },
#endif
#if CONFIG_HEAP_POISONING_COMPREHENSIVE || CONFIG_HEAP_POISONING_LIGHT
        { .command = "heapwatch", .help = "Debug: check heap integrity in the background, abort on corruption. "
                                          "Internal heap every ms (0 = off, default 200), PSRAM every "
                                          "psram_s (default 10, 0 = never; ~100 ms stall each)",
          .hint = "[ms [psram_s]]", .func = cmd_heapwatch },
#endif
        { .command = "reboot", .help = "Restart the device", .func = cmd_reboot },
    };

    ESP_RETURN_ON_ERROR(esp_console_register_help_command(), TAG, "register help");
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        ESP_RETURN_ON_ERROR(esp_console_cmd_register(&cmds[i]), TAG, "register %s", cmds[i].command);
    }
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
    ESP_RETURN_ON_ERROR(console_coredump_register(), TAG, "register coredump");
#endif

    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_cfg.prompt = PROMPT;
    repl_cfg.task_stack_size = 6144;    /* commands keep a phone_status_t (~1 KB) on the stack */

    /* Picks whichever console is configured (UART, USB-serial-JTAG, USB CDC). */
    ESP_RETURN_ON_ERROR(esp_console_new_repl_stdio(&repl_cfg, &repl), TAG, "create repl");
    linenoiseSetReadFunction(console_read);     /* after the REPL installed its own */
    ESP_RETURN_ON_ERROR(esp_console_start_repl(repl), TAG, "start repl");
    return ESP_OK;
}
