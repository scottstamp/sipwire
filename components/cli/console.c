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
    FIELD(ring_volume, F_U8, 0, 100),
    FIELD(call_volume, F_U8, 0, 100),
    FIELD(mic_gain, F_U8, 0, 100),
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

static int cmd_cfg(int argc, char **argv)
{
    settings_t s;

    if (argc == 2 && strcmp(argv[1], "show") == 0) {
        settings_get(&s);
        for (size_t i = 0; i < NUM_FIELDS; i++) {
            field_print(&s_fields[i], &s);
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
        settings_get(&s);
        if (!field_parse(f, &s, argc == 4 ? argv[3] : "")) {
            return 1;
        }
        esp_err_t err = settings_set(&s);
        if (err != ESP_OK) {
            printf("rejected: %s\n", esp_err_to_name(err));
            return 1;
        }
        settings_get(&s);
        field_print(f, &s);
        return 0;
    }
    printf("usage: cfg show | cfg set <key> [value] | cfg reset\n");
    return 1;
}

/* ---- other built-ins ---- */

static int cmd_wifi(int argc, char **argv)
{
    if (argc < 2 || argc > 3) {
        printf("usage: wifi <ssid> [pass]\n");
        return 1;
    }
    settings_t s;
    settings_get(&s);
    if (strlen(argv[1]) >= sizeof(s.wifi_ssid) || (argc == 3 && strlen(argv[2]) >= sizeof(s.wifi_pass))) {
        printf("ssid max %u chars, password max %u chars\n", (unsigned)sizeof(s.wifi_ssid) - 1,
               (unsigned)sizeof(s.wifi_pass) - 1);
        return 1;
    }
    strcpy(s.wifi_ssid, argv[1]);
    strcpy(s.wifi_pass, argc == 3 ? argv[2] : "");
    esp_err_t err = settings_set(&s);
    if (err != ESP_OK) {
        printf("rejected: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("saved ssid '%s'%s, connecting\n", s.wifi_ssid, argc == 3 ? "" : " (open network)");
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
    printf("name            state prio stack-hwm num\n");
    vTaskList(buf);
    printf("%s", buf);
    free(buf);
    return 0;
}
#endif

#if CONFIG_FREERTOS_USE_TRACE_FACILITY && CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
#define CPU_MAX_TASKS 40

static TaskStatus_t *cpu_snapshot(UBaseType_t *n, configRUN_TIME_COUNTER_TYPE *total)
{
    TaskStatus_t *st = calloc(CPU_MAX_TASKS, sizeof(*st));
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

static int cmd_reboot(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    printf("rebooting\n");
    fflush(stdout);
    esp_restart();
    return 0;
}

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
        if (n > 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
            return n;
        }
    }
}

/* ---- public API ---- */

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
        { .command = "reboot", .help = "Restart the device", .func = cmd_reboot },
    };

    ESP_RETURN_ON_ERROR(esp_console_register_help_command(), TAG, "register help");
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        ESP_RETURN_ON_ERROR(esp_console_cmd_register(&cmds[i]), TAG, "register %s", cmds[i].command);
    }

    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_cfg.prompt = PROMPT;
    repl_cfg.task_stack_size = 6144;    /* cfg show keeps a settings_t on the stack */

    /* Picks whichever console is configured (UART, USB-serial-JTAG, USB CDC). */
    ESP_RETURN_ON_ERROR(esp_console_new_repl_stdio(&repl_cfg, &repl), TAG, "create repl");
    linenoiseSetReadFunction(console_read);     /* after the REPL installed its own */
    ESP_RETURN_ON_ERROR(esp_console_start_repl(repl), TAG, "start repl");
    return ESP_OK;
}
