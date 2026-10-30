/* `coredump` console command: the core dump a panic left in the `coredump` flash partition
 * (CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH, docs/ARCHITECTURE.md "Core dumps"). */
#include "sdkconfig.h"

#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_core_dump.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "console.h"

static const char *TAG = "console";

#define ELF_HINT "build/sipwire.elf"

static const esp_partition_t *coredump_partition(void)
{
    return esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, NULL);
}

#if CONFIG_IDF_TARGET_ARCH_XTENSA
/* EXCCAUSE values (Xtensa ISA), named as the panic handler prints them */
static const char *exc_cause_str(uint32_t cause)
{
    static const char *const names[] = {
        [0] = "IllegalInstruction",       [1] = "Syscall",
        [2] = "InstructionFetchError",    [3] = "LoadStoreError",
        [4] = "Level1Interrupt",          [5] = "Alloca",
        [6] = "IntegerDivideByZero",      [8] = "Privileged",
        [9] = "LoadStoreAlignment",       [12] = "InstrPIFDataError",
        [13] = "LoadStorePIFDataError",   [14] = "InstrPIFAddrError",
        [15] = "LoadStorePIFAddrError",   [16] = "InstTLBMiss",
        [17] = "InstTLBMultiHit",         [18] = "InstFetchPrivilege",
        [20] = "InstFetchProhibited",     [24] = "LoadStoreTLBMiss",
        [25] = "LoadStoreTLBMultihit",    [26] = "LoadStorePrivilege",
        [28] = "LoadProhibited",          [29] = "StoreProhibited",
    };
    if (cause >= 32 && cause <= 39) {
        return "CoprocessorDisabled";
    }
    if (cause < sizeof(names) / sizeof(names[0]) && names[cause] != NULL) {
        return names[cause];
    }
    return "?";
}
#endif

/* Summary of the stored dump (ESP_OK; checksum verified), or why there is none. Callers check
 * that the partition exists. */
static esp_err_t summary_get(esp_core_dump_summary_t **out, size_t *size)
{
    *out = NULL;
    size_t addr = 0;
    esp_err_t err = esp_core_dump_image_get(&addr, size);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_core_dump_image_check();
    if (err != ESP_OK) {
        return err;
    }
    esp_core_dump_summary_t *s = heap_caps_malloc_prefer(sizeof(*s), 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                                         MALLOC_CAP_8BIT);
    if (s == NULL) {
        return ESP_ERR_NO_MEM;
    }
    err = esp_core_dump_get_summary(s);
    if (err != ESP_OK) {
        free(s);
        return err;
    }
    *out = s;
    return ESP_OK;
}

static int coredump_show(void)
{
    const esp_partition_t *part = coredump_partition();
    if (part == NULL) {
        printf("no coredump partition: this unit has an older partition table, crashes are not saved.\n"
               "Flash it over serial once (idf.py -p PORT flash; README \"Build\").\n");
        return 1;
    }
    esp_core_dump_summary_t *s = NULL;
    size_t size = 0;
    esp_err_t err = summary_get(&s, &size);
    if (err == ESP_ERR_NOT_FOUND || err == ESP_ERR_INVALID_SIZE) {
        /* blank partition, or the erased marker / foreign data in it */
        printf("no core dump (partition '%s' @ 0x%" PRIx32 ", %" PRIu32 " bytes)\n", part->label, part->address,
               part->size);
        return 0;
    }
    if (err != ESP_OK) {
        printf("core dump of %u bytes is unreadable: %s (`coredump erase` clears it)\n", (unsigned)size,
               esp_err_to_name(err));
        return 1;
    }
    printf("core dump: %u of %" PRIu32 " bytes in partition '%s' @ 0x%" PRIx32 ", checksum ok\n", (unsigned)size,
           part->size, part->label, part->address);
    char *reason = malloc(200);
    if (reason != NULL && esp_core_dump_get_panic_reason(reason, 200) == ESP_OK) {
        printf("reason:    %s\n", reason);
    }
    free(reason);
    printf("task:      %.*s (TCB 0x%08" PRIx32 ")\n", (int)sizeof(s->exc_task), s->exc_task, s->exc_tcb);
    printf("pc:        0x%08" PRIx32 "\n", s->exc_pc);
#if CONFIG_IDF_TARGET_ARCH_XTENSA
    printf("exception: %" PRIu32 " (%s), vaddr 0x%08" PRIx32 "\n", s->ex_info.exc_cause,
           exc_cause_str(s->ex_info.exc_cause), s->ex_info.exc_vaddr);
    uint32_t depth = s->exc_bt_info.depth;
    if (depth > sizeof(s->exc_bt_info.bt) / sizeof(s->exc_bt_info.bt[0])) {
        depth = sizeof(s->exc_bt_info.bt) / sizeof(s->exc_bt_info.bt[0]);
    }
    printf("backtrace:");
    for (uint32_t i = 0; i < depth; i++) {
        printf(" 0x%08" PRIx32, s->exc_bt_info.bt[i]);
    }
    printf("%s\n", s->exc_bt_info.corrupted ? " |<-CORRUPTED" : "");
    /* the backtrace starts at the PC */
    printf("decode:    xtensa-esp32s3-elf-addr2line -pfiaC -e " ELF_HINT);
    if (depth == 0) {
        printf(" 0x%08" PRIx32, s->exc_pc);
    }
    for (uint32_t i = 0; i < depth; i++) {
        printf(" 0x%08" PRIx32, s->exc_bt_info.bt[i]);
    }
    printf("\n");
#endif
    printf("elf sha256: %.*s... (decode with the ELF of that build: its boot log prints the same "
           "\"ELF file SHA256\")\n", (int)strnlen((const char *)s->app_elf_sha256, sizeof(s->app_elf_sha256)),
           (const char *)s->app_elf_sha256);
    printf("full dump: `coredump dump`, or over USB `idf.py coredump-info` (reads the partition)\n");
    free(s);
    return 0;
}

static int log_discard(const char *fmt, va_list ap)
{
    (void)fmt;
    (void)ap;
    return 0;
}

/* The stored image (header + ELF + checksum) in base64, 64 characters (48 bytes) per line:
 * what CONFIG_ESP_COREDUMP_ENABLE_TO_UART would print. Save the lines between the markers to
 * a file, then `idf.py coredump-info -c <file>` (esp-coredump detects base64; it decodes line by
 * line, so a log line inside the block breaks it: logs are dropped while the dump prints). */
static int coredump_dump(void)
{
    static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const esp_partition_t *part = coredump_partition();
    size_t addr = 0, size = 0;
    if (part == NULL || esp_core_dump_image_get(&addr, &size) != ESP_OK) {
        printf("no core dump\n");
        return 1;
    }
    fflush(stdout);
    vprintf_like_t prev_log = esp_log_set_vprintf(log_discard);
    printf("================= CORE DUMP START =================\n");
    uint8_t in[48];
    char line[65];
    esp_err_t err = ESP_OK;
    for (size_t off = 0; off < size && err == ESP_OK; off += sizeof(in)) {
        size_t n = size - off < sizeof(in) ? size - off : sizeof(in);
        err = esp_partition_read(part, off, in, n);
        if (err != ESP_OK) {
            printf("\nread at %u: %s\n", (unsigned)off, esp_err_to_name(err));
            break;
        }
        size_t o = 0;
        for (size_t i = 0; i < n; i += 3) {
            uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < n ? (uint32_t)in[i + 1] << 8 : 0) |
                         (i + 2 < n ? in[i + 2] : 0);
            line[o++] = b64[v >> 18 & 63];
            line[o++] = b64[v >> 12 & 63];
            line[o++] = i + 1 < n ? b64[v >> 6 & 63] : '=';
            line[o++] = i + 2 < n ? b64[v & 63] : '=';
        }
        line[o] = '\0';
        printf("%s\n", line);
    }
    printf("================= CORE DUMP END ===================\n");
    fflush(stdout);
    esp_log_set_vprintf(prev_log);
    if (err != ESP_OK) {
        return 1;
    }
    printf("%u bytes: save the lines between the markers as core.b64, then\n"
           "idf.py coredump-info -c core.b64   (with the ELF of the crashed build in build/)\n", (unsigned)size);
    return 0;
}

static void __attribute__((noinline)) coredump_crash(void)
{
    volatile uintptr_t bad = 4;     /* a run-time value: no compile-time null dereference warning */
    printf("crashing (store to an invalid address)\n");
    fflush(stdout);
    *(volatile uint32_t *)bad = 0;
}

static int cmd_coredump(int argc, char **argv)
{
    if (argc == 1) {
        return coredump_show();
    }
    if (argc == 2 && strcmp(argv[1], "erase") == 0) {
        esp_err_t err = coredump_partition() ? esp_core_dump_image_erase() : ESP_ERR_NOT_FOUND;
        printf("%s\n", err == ESP_OK ? "core dump erased" : esp_err_to_name(err));
        return err == ESP_OK ? 0 : 1;
    }
    if (argc == 2 && strcmp(argv[1], "dump") == 0) {
        return coredump_dump();
    }
    if (argc == 2 && strcmp(argv[1], "crash") == 0) {
        coredump_crash();
        return 1;
    }
    printf("usage: coredump [erase | dump | crash]\n");
    return 1;
}

esp_err_t console_coredump_register(void)
{
    const esp_partition_t *part = coredump_partition();
    size_t addr = 0, size = 0;
    if (part == NULL) {
        ESP_LOGW(TAG, "no coredump partition (old partition table): crashes are not saved; flash over serial once");
    } else if (esp_core_dump_image_get(&addr, &size) == ESP_OK) {
        esp_core_dump_summary_t *s = NULL;
        if (summary_get(&s, &size) == ESP_OK) {
            ESP_LOGW(TAG, "core dump in flash: crash in task '%.*s' at pc 0x%08" PRIx32 " - `coredump` for details",
                     (int)sizeof(s->exc_task), s->exc_task, s->exc_pc);
            free(s);
        } else {
            ESP_LOGW(TAG, "core dump in flash (%u bytes, not readable) - `coredump` for details", (unsigned)size);
        }
    }
    static const esp_console_cmd_t cmd = {
        .command = "coredump",
        .help = "Crash dump saved by the last panic: summary (task, PC, backtrace for addr2line); "
                "`erase` clears it, `dump` prints it in base64 for `idf.py coredump-info -c`, "
                "`crash` panics now (test)",
        .hint = "[erase | dump | crash]",
        .func = cmd_coredump,
    };
    return esp_console_cmd_register(&cmd);
}

#endif /* CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH */
