/* Console commands: contact, speed, history. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>

#include "console.h"
#include "dir_core.h"
#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "directory";

#define IMPORT_MAX   (24 * 1024)
#define IMPORT_IDLE  60     /* seconds without a byte before `contact import` gives up */

/* The mutators only queue the file write (directory.h): the console waits for it, so what it
 * prints is on flash (and a save error is reported). */
#define FLUSH_MS 10000

static esp_err_t saved(esp_err_t err)
{
    return err == ESP_OK ? dir_flush(FLUSH_MS) : err;
}

static void print_contacts(const dir_contact_t *c, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        printf("%-20s %s\n", c[i].number, c[i].name);
    }
}

static dir_contact_t *alloc_contacts(void)
{
    return malloc(DIR_CONTACTS_MAX * sizeof(dir_contact_t));
}

/* Reads one byte from the console; false on timeout. The REPL's stdin is non-blocking. */
static bool console_getc(char *c, int timeout_s)
{
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(STDIN_FILENO, &rfds);
    struct timeval tv = { .tv_sec = timeout_s };
    if (select(STDIN_FILENO + 1, &rfds, NULL, NULL, &tv) <= 0) {
        return false;
    }
    return read(STDIN_FILENO, c, 1) == 1;
}

static int contact_import(void)
{
    char *text = malloc(IMPORT_MAX + 1);
    if (text == NULL) {
        printf("out of memory\n");
        return 1;
    }
    printf("paste CSV lines (number,name), then an empty line (or '.'); Ctrl-C aborts\n");
    size_t len = 0, line_start = 0;
    bool any = false;
    for (;;) {
        char c;
        if (!console_getc(&c, IMPORT_IDLE)) {
            printf("import: timed out, nothing imported\n");
            free(text);
            return 1;
        }
        if (c == 0x03) {
            printf("import: aborted\n");
            free(text);
            return 1;
        }
        if (c == '\r') {
            continue;
        }
        if (c == '\n') {
            size_t l = len - line_start;
            bool dot = l == 1 && text[line_start] == '.';
            if (dot || (l == 0 && any)) {
                len = dot ? line_start : len;
                break;
            }
            /* empty lines before the first data line are the Enter of the command itself */
            if (l > 0) {
                any = true;
                text[len++] = '\n';
                line_start = len;
            }
            continue;
        }
        if (len >= IMPORT_MAX) {
            printf("import: more than %d bytes, nothing imported\n", IMPORT_MAX);
            free(text);
            return 1;
        }
        text[len++] = c;
    }
    size_t added = 0, bad = 0;
    esp_err_t err = saved(dir_contact_import_csv(text, len, &added, &bad));
    free(text);
    printf("imported %u contacts, %u bad lines, %u total%s%s\n", (unsigned)added, (unsigned)bad,
           (unsigned)dir_contact_count(), err == ESP_OK ? "" : "; save failed: ",
           err == ESP_OK ? "" : esp_err_to_name(err));
    return err == ESP_OK ? 0 : 1;
}

static int cmd_contact(int argc, char **argv)
{
    if (argc >= 4 && strcmp(argv[1], "add") == 0) {
        char name[DIR_NAME_MAX * 2] = "";
        for (int i = 3; i < argc; i++) {
            snprintf(name + strlen(name), sizeof(name) - strlen(name), "%s%s", i > 3 ? " " : "", argv[i]);
        }
        esp_err_t err = saved(dir_contact_set(argv[2], name));
        if (err != ESP_OK) {
            printf("contact add failed: %s\n", esp_err_to_name(err));
            return 1;
        }
        char num[DIR_NUMBER_MAX], stored[DIR_NAME_MAX];
        dir_normalize(argv[2], num, sizeof(num));
        dir_contact_lookup(num, stored, sizeof(stored));
        printf("contact %s = %s (%u total)\n", num, stored, (unsigned)dir_contact_count());
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "del") == 0) {
        esp_err_t err = saved(dir_contact_del(argv[2]));
        if (err != ESP_OK) {
            printf("contact del failed: %s\n", esp_err_to_name(err));
            return 1;
        }
        printf("deleted (%u left)\n", (unsigned)dir_contact_count());
        return 0;
    }
    if ((argc == 2 || argc == 3) && strcmp(argv[1], "list") == 0) {
        dir_contact_t *c = alloc_contacts();
        if (c == NULL) {
            return 1;
        }
        size_t n = dir_contact_search(argc == 3 ? argv[2] : "", c, DIR_CONTACTS_MAX);
        print_contacts(c, n);
        printf("%u of %u contacts\n", (unsigned)n, (unsigned)dir_contact_count());
        free(c);
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "export") == 0) {
        dir_contact_t *c = alloc_contacts();
        if (c == NULL) {
            return 1;
        }
        size_t n = dir_contact_list(c, 0, DIR_CONTACTS_MAX);
        for (size_t i = 0; i < n; i++) {
            char line[DIR_LINE_MAX];
            dir_contact_csv_line(&c[i], line, sizeof(line));
            printf("%s\n", line);
        }
        free(c);
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "import") == 0) {
        return contact_import();
    }
    printf("usage: contact add <number> <name...> | del <number> | list [query] | export | import\n");
    return 1;
}

static int cmd_speed(int argc, char **argv)
{
    if (argc == 1) {
        for (int d = DIR_SPEED_FIRST; d <= DIR_SPEED_LAST; d++) {
            char num[DIR_NUMBER_MAX], name[DIR_NAME_MAX];
            if (dir_speed_get(d, num, sizeof(num)) == ESP_OK) {
                dir_contact_lookup(num, name, sizeof(name));
                printf("%d  %-20s %s\n", d, num, name);
            } else {
                printf("%d  -\n", d);
            }
        }
        return 0;
    }
    int d = atoi(argv[1]);
    if (strlen(argv[1]) != 1 || d < DIR_SPEED_FIRST || d > DIR_SPEED_LAST) {
        printf("digit must be %d..%d (1 is voicemail, see cfg vm_number)\n", DIR_SPEED_FIRST, DIR_SPEED_LAST);
        return 1;
    }
    if (argc == 3) {
        esp_err_t err = saved(dir_speed_set(d, strcmp(argv[2], "-") == 0 ? "" : argv[2]));
        if (err != ESP_OK) {
            printf("speed failed: %s\n", esp_err_to_name(err));
            return 1;
        }
    }
    char num[DIR_NUMBER_MAX];
    if (dir_speed_get(d, num, sizeof(num)) == ESP_OK) {
        printf("speed %d = %s\n", d, num);
    } else {
        printf("speed %d = (empty)\n", d);
    }
    return 0;
}

static int cmd_history(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "clear") == 0) {
        esp_err_t err = saved(dir_history_clear());
        printf("history cleared%s%s\n", err == ESP_OK ? "" : "; save failed: ",
               err == ESP_OK ? "" : esp_err_to_name(err));
        return err == ESP_OK ? 0 : 1;
    }
    if (argc == 2 && strcmp(argv[1], "seen") == 0) {
        dir_history_mark_seen();
        printf("missed calls marked seen\n");
        return 0;
    }
    static const char *const res[] = { "answered", "missed", "rejected", "dnd" };
    if (argc >= 5 && argc <= 6 && strcmp(argv[1], "add") == 0) {
        /* test aid until phone.c logs calls itself: history add <in|out> <result> <number> [duration_s] */
        dir_call_t e = { .start = time(NULL), .duration_s = argc == 6 ? (uint32_t)atoi(argv[5]) : 0 };
        e.direction = strcmp(argv[2], "out") == 0 ? DIR_CALL_OUT : DIR_CALL_IN;
        for (uint8_t r = 0; r < 4; r++) {
            if (strcmp(argv[3], res[r]) == 0) {
                e.result = r;
            }
        }
        snprintf(e.number, sizeof(e.number), "%s", argv[4]);
        esp_err_t err = saved(dir_history_add(&e));
        printf("history add: %s\n", esp_err_to_name(err));
        return err == ESP_OK ? 0 : 1;
    }
    if (argc != 1) {
        printf("usage: history [clear | seen | add <in|out> <answered|missed|rejected|dnd> <number> [secs]]\n");
        return 1;
    }
    dir_call_t *c = malloc(DIR_HISTORY_MAX * sizeof(*c));
    if (c == NULL) {
        return 1;
    }
    size_t n = dir_history_list(c, 0, DIR_HISTORY_MAX);
    for (size_t i = 0; i < n; i++) {
        char when[20] = "--";
        if (c[i].start > 0) {
            struct tm tm;
            localtime_r(&c[i].start, &tm);
            strftime(when, sizeof(when), "%m-%d %H:%M", &tm);
        }
        printf("%-11s %-3s %-8s %-4s %4us %-16s %s\n", when, c[i].direction == DIR_CALL_IN ? "in" : "out",
               res[c[i].result & 3], c[i].seen ? "" : "NEW", (unsigned)c[i].duration_s, c[i].number, c[i].name);
    }
    printf("%u calls, %u unseen missed\n", (unsigned)n, dir_history_unseen_missed());
    free(c);
    return 0;
}

esp_err_t dir_console_register(void)
{
    static const esp_console_cmd_t cmds[] = {
        { .command = "contact", .help = "Contacts. `contact import` reads CSV lines (number,name) until an empty line",
          .hint = "add <number> <name...> | del <number> | list [query] | export | import", .func = cmd_contact },
        { .command = "speed", .help = "Speed dial keys 2-9: show all, show one, set (number) or clear (-)",
          .hint = "[digit [number|-]]", .func = cmd_speed },
        { .command = "history", .help = "Call history, newest first", .hint = "[clear | seen | add <in|out> <result> <number> [secs]]", .func = cmd_history },
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        ESP_RETURN_ON_ERROR(console_register_command(&cmds[i]), TAG, "register %s", cmds[i].command);
    }
    return ESP_OK;
}
