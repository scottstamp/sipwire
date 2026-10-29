#include "directory.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "dir_core.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "wear_levelling.h"

static const char *TAG = "directory";

#define MOUNT_POINT   "/data"
#define PARTITION     "storage"
#define F_CONTACTS    MOUNT_POINT "/contacts.csv"
#define F_CONTACTS_T  MOUNT_POINT "/contacts.tmp"
#define F_SPEED       MOUNT_POINT "/speed.csv"
#define F_SPEED_T     MOUNT_POINT "/speed.tmp"
#define F_HISTORY     MOUNT_POINT "/history.bin"
#define F_HISTORY_T   MOUNT_POINT "/history.tmp"
#define FILE_MAX      (64 * 1024)   /* refuse to load anything larger (corrupt / foreign file) */

static SemaphoreHandle_t s_lock;
static bool s_mounted;
static wl_handle_t s_wl = WL_INVALID_HANDLE;
static dir_ctab_t s_contacts;
static dir_speed_tab_t s_speed;
static dir_hist_t s_hist;
/* History changes come from the sip task (call end, small stack, must not stall on flash) and the
 * LVGL task: they only mark the history dirty, and this task writes it (s_lock is held just for
 * the in-RAM serialisation, not for the flash write). */
static TaskHandle_t s_writer;
static bool s_hist_dirty;           /* under s_lock */
#define WRITER_COALESCE_MS  200     /* several changes in a row (mark seen + clear...) = one write */

esp_err_t dir_console_register(void);    /* dir_console.c */

void dir_normalize(const char *in, char *out, size_t cap)
{
    dir_core_normalize(in, out, cap);
}

/* ---- storage ---- */

static void *big_calloc(size_t n, size_t size)
{
    void *p = heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : heap_caps_calloc(n, size, MALLOC_CAP_8BIT);
}

static char *read_file(const char *path, size_t *len)
{
    struct stat st;
    if (stat(path, &st) != 0 || st.st_size <= 0 || st.st_size > FILE_MAX) {
        return NULL;
    }
    char *buf = heap_caps_malloc((size_t)st.st_size + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        buf = heap_caps_malloc((size_t)st.st_size + 1, MALLOC_CAP_8BIT);
    }
    FILE *f = buf ? fopen(path, "rb") : NULL;
    if (f == NULL) {
        free(buf);
        return NULL;
    }
    size_t n = fread(buf, 1, (size_t)st.st_size, f);
    fclose(f);
    buf[n] = '\0';
    *len = n;
    return buf;
}

/* FAT cannot rename over an existing file: replace = unlink + rename. The temp file stays
 * complete on disk (it ends with the "#end" trailer / has a CRC), so loading falls back to it
 * if power fails between the two calls. */
static esp_err_t replace_file(const char *tmp, const char *path)
{
    unlink(path);
    if (rename(tmp, path) != 0) {
        ESP_LOGE(TAG, "rename %s: errno %d", path, errno);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t save_text(const char *path, const char *tmp, int (*writer)(const void *, FILE *), const void *obj)
{
    if (!s_mounted) {
        return ESP_ERR_INVALID_STATE;
    }
    FILE *f = fopen(tmp, "w");
    if (f == NULL) {
        ESP_LOGE(TAG, "open %s: errno %d", tmp, errno);
        return ESP_FAIL;
    }
    int rc = writer(obj, f);
    if (fclose(f) != 0 || rc != 0) {
        ESP_LOGE(TAG, "write %s failed", tmp);
        unlink(tmp);
        return ESP_FAIL;
    }
    return replace_file(tmp, path);
}

static int write_contacts(const void *obj, FILE *f)
{
    return dir_ctab_write(obj, f);
}

static int write_speed(const void *obj, FILE *f)
{
    return dir_speed_write(obj, f);
}

static esp_err_t save_contacts(void)
{
    return save_text(F_CONTACTS, F_CONTACTS_T, write_contacts, &s_contacts);
}

static esp_err_t save_speed(void)
{
    return save_text(F_SPEED, F_SPEED_T, write_speed, &s_speed);
}

/* Caller holds s_lock: serialise the history into a new buffer (PSRAM if possible). */
static uint8_t *history_snapshot_locked(size_t *len)
{
    size_t need = dir_hist_size(&s_hist);
    uint8_t *buf = heap_caps_malloc(need, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        buf = heap_caps_malloc(need, MALLOC_CAP_8BIT);
    }
    if (buf != NULL) {
        *len = dir_hist_serialize(&s_hist, buf, need);
    }
    return buf;
}

static esp_err_t write_history(const uint8_t *buf, size_t len)
{
    esp_err_t err = ESP_FAIL;
    FILE *f = fopen(F_HISTORY_T, "wb");
    if (f) {
        bool ok = fwrite(buf, 1, len, f) == len;
        ok = (fclose(f) == 0) && ok;
        err = ok ? replace_file(F_HISTORY_T, F_HISTORY) : ESP_FAIL;
        if (!ok) {
            unlink(F_HISTORY_T);
        }
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save history failed");
    }
    return err;
}

static void writer_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        vTaskDelay(pdMS_TO_TICKS(WRITER_COALESCE_MS));
        uint8_t *buf = NULL;
        size_t len = 0;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_hist_dirty) {
            buf = history_snapshot_locked(&len);
            s_hist_dirty = buf == NULL;     /* out of memory: retry on the next change */
        }
        xSemaphoreGive(s_lock);
        if (buf != NULL) {
            write_history(buf, len);
            free(buf);
        }
    }
}

/* Caller holds s_lock. */
static esp_err_t save_history(void)
{
    if (!s_mounted || s_writer == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    s_hist_dirty = true;
    xTaskNotifyGive(s_writer);
    return ESP_OK;
}

static void load_contacts(void)
{
    size_t len, bad = 0;
    bool complete = false;
    char *text = read_file(F_CONTACTS, &len);
    if (text) {
        size_t n = dir_ctab_parse(&s_contacts, text, len, &bad, &complete);
        ESP_LOGI(TAG, "contacts: %u loaded, %u bad lines", (unsigned)n, (unsigned)bad);
        free(text);
        return;
    }
    text = read_file(F_CONTACTS_T, &len);     /* interrupted replace */
    if (text) {
        dir_ctab_parse(&s_contacts, text, len, &bad, &complete);
        if (!complete) {
            s_contacts.n = 0;   /* half-written temp file: not trustworthy */
        } else {
            ESP_LOGW(TAG, "contacts: recovered %u from the temp file", (unsigned)s_contacts.n);
        }
        free(text);
    }
}

static void load_speed(void)
{
    size_t len;
    bool complete = false;
    char *text = read_file(F_SPEED, &len);
    if (text) {
        dir_speed_parse(&s_speed, text, len, &complete);
        free(text);
        return;
    }
    text = read_file(F_SPEED_T, &len);
    if (text) {
        dir_speed_parse(&s_speed, text, len, &complete);
        if (!complete) {
            memset(&s_speed, 0, sizeof(s_speed));
        }
        free(text);
    }
}

static void load_history(void)
{
    const char *paths[] = { F_HISTORY, F_HISTORY_T };
    for (int i = 0; i < 2; i++) {
        size_t len;
        char *buf = read_file(paths[i], &len);
        if (buf == NULL) {
            continue;
        }
        int rc = dir_hist_deserialize(&s_hist, (const uint8_t *)buf, len);
        free(buf);
        if (rc == 0) {
            ESP_LOGI(TAG, "history: %u entries", (unsigned)s_hist.count);
            return;
        }
        ESP_LOGW(TAG, "%s is corrupt, ignored", paths[i]);
    }
}

esp_err_t dir_init(void)
{
    if (s_lock != NULL) {
        return ESP_OK;
    }
    s_lock = xSemaphoreCreateMutex();
    s_contacts = (dir_ctab_t){ .v = big_calloc(DIR_CONTACTS_MAX, sizeof(dir_contact_t)), .cap = DIR_CONTACTS_MAX };
    s_hist = (dir_hist_t){ .e = big_calloc(DIR_HISTORY_MAX, sizeof(dir_call_t)), .cap = DIR_HISTORY_MAX };
    ESP_RETURN_ON_FALSE(s_lock && s_contacts.v && s_hist.e, ESP_ERR_NO_MEM, TAG, "no memory");

    /* 4 KB sectors (CONFIG_WL_SECTOR_SIZE); a failed first mount formats the partition. */
    const esp_vfs_fat_mount_config_t mc = {
        .format_if_mount_failed = true,
        .max_files = 3,
        .allocation_unit_size = CONFIG_WL_SECTOR_SIZE,
    };
    size_t int_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t psram_before = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl(MOUNT_POINT, PARTITION, &mc, &s_wl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mount /data (%s): %s - contacts and history are not saved", PARTITION, esp_err_to_name(err));
    } else {
        s_mounted = true;
        ESP_LOGI(TAG, "mounted /data: FATFS+WL use %d B internal, %d B PSRAM", (int)(int_before - heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                 (int)(psram_before - heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
        load_contacts();
        load_speed();
        load_history();
        /* internal-RAM stack: it runs FATFS / flash writes */
        if (xTaskCreate(writer_task, "dir_save", 4096, NULL, 2, &s_writer) != pdPASS) {
            ESP_LOGE(TAG, "no memory for the writer task - history is not saved");
        }
    }
    dir_console_register();
    return ESP_OK;
}

/* ---- contacts ---- */

esp_err_t dir_contact_set(const char *number, const char *name)
{
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_INVALID_STATE, TAG, "not initialised");
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int r = dir_ctab_set(&s_contacts, number, name);
    esp_err_t err = r == DIR_C_INVALID ? ESP_ERR_INVALID_ARG : r == DIR_C_FULL ? ESP_ERR_NO_MEM : ESP_OK;
    if (err == ESP_OK) {
        err = save_contacts();
    }
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t dir_contact_del(const char *number)
{
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_INVALID_STATE, TAG, "not initialised");
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = dir_ctab_del(&s_contacts, number) ? save_contacts() : ESP_ERR_NOT_FOUND;
    xSemaphoreGive(s_lock);
    return err;
}

size_t dir_contact_count(void)
{
    if (s_lock == NULL) {
        return 0;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    size_t n = s_contacts.n;
    xSemaphoreGive(s_lock);
    return n;
}

size_t dir_contact_list(dir_contact_t *out, size_t start, size_t max)
{
    if (s_lock == NULL || out == NULL) {
        return 0;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    size_t n = 0;
    for (size_t i = start; i < s_contacts.n && n < max; i++) {
        out[n++] = s_contacts.v[i];
    }
    xSemaphoreGive(s_lock);
    return n;
}

size_t dir_contact_search(const char *query, dir_contact_t *out, size_t max)
{
    if (s_lock == NULL || out == NULL) {
        return 0;
    }
    size_t *idx = max ? malloc(max * sizeof(size_t)) : NULL;
    if (idx == NULL) {
        return 0;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    size_t n = dir_ctab_search(&s_contacts, query, idx, max);
    for (size_t i = 0; i < n; i++) {
        out[i] = s_contacts.v[idx[i]];
    }
    xSemaphoreGive(s_lock);
    free(idx);
    return n;
}

bool dir_contact_lookup(const char *number, char *name, size_t name_cap)
{
    if (s_lock == NULL || name == NULL || name_cap == 0) {
        return false;
    }
    name[0] = '\0';
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int i = dir_ctab_find(&s_contacts, number);
    if (i >= 0) {
        snprintf(name, name_cap, "%s", s_contacts.v[i].name);
    }
    xSemaphoreGive(s_lock);
    return i >= 0;
}

esp_err_t dir_contact_import_csv(const char *text, size_t len, size_t *added, size_t *bad)
{
    ESP_RETURN_ON_FALSE(s_lock && text, ESP_ERR_INVALID_STATE, TAG, "not initialised");
    xSemaphoreTake(s_lock, portMAX_DELAY);
    size_t b = 0;
    size_t n = dir_ctab_parse(&s_contacts, text, len, &b, NULL);
    esp_err_t err = n ? save_contacts() : ESP_OK;
    xSemaphoreGive(s_lock);
    if (added) {
        *added = n;
    }
    if (bad) {
        *bad = b;
    }
    return err;
}

size_t dir_contact_csv_line(const dir_contact_t *c, char *buf, size_t cap)
{
    return dir_csv_format_line(c, buf, cap);
}

/* ---- speed dial ---- */

esp_err_t dir_speed_set(int digit, const char *number)
{
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_INVALID_STATE, TAG, "not initialised");
    ESP_RETURN_ON_FALSE(digit >= DIR_SPEED_FIRST && digit <= DIR_SPEED_LAST, ESP_ERR_INVALID_ARG, TAG, "digit");
    xSemaphoreTake(s_lock, portMAX_DELAY);
    dir_speed_set_tab(&s_speed, digit, number);
    esp_err_t err = save_speed();
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t dir_speed_get(int digit, char *number, size_t cap)
{
    ESP_RETURN_ON_FALSE(s_lock && number && cap, ESP_ERR_INVALID_STATE, TAG, "not initialised");
    ESP_RETURN_ON_FALSE(digit >= DIR_SPEED_FIRST && digit <= DIR_SPEED_LAST, ESP_ERR_INVALID_ARG, TAG, "digit");
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const char *n = s_speed.num[digit - DIR_SPEED_FIRST];
    esp_err_t err = n[0] ? ESP_OK : ESP_ERR_NOT_FOUND;
    snprintf(number, cap, "%s", n);
    xSemaphoreGive(s_lock);
    return err;
}

/* ---- history ---- */

esp_err_t dir_history_add(const dir_call_t *c)
{
    ESP_RETURN_ON_FALSE(s_lock && c, ESP_ERR_INVALID_STATE, TAG, "not initialised");
    dir_call_t e = *c;
    dir_core_normalize(c->number, e.number, sizeof(e.number));
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (e.name[0] == '\0') {
        int i = dir_ctab_find(&s_contacts, e.number);
        if (i >= 0) {
            snprintf(e.name, sizeof(e.name), "%s", s_contacts.v[i].name);
        }
    }
    e.seen = !(e.direction == DIR_CALL_IN && e.result == DIR_RES_MISSED);
    dir_hist_add(&s_hist, &e);
    esp_err_t err = save_history();
    xSemaphoreGive(s_lock);
    return err;
}

size_t dir_history_count(void)
{
    if (s_lock == NULL) {
        return 0;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    size_t n = s_hist.count;
    xSemaphoreGive(s_lock);
    return n;
}

size_t dir_history_list(dir_call_t *out, size_t start, size_t max)
{
    if (s_lock == NULL || out == NULL) {
        return 0;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    size_t n = 0;
    for (const dir_call_t *c; n < max && (c = dir_hist_get(&s_hist, start + n)) != NULL; n++) {
        out[n] = *c;
    }
    xSemaphoreGive(s_lock);
    return n;
}

esp_err_t dir_history_clear(void)
{
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_INVALID_STATE, TAG, "not initialised");
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_hist.head = 0;
    s_hist.count = 0;
    esp_err_t err = save_history();
    xSemaphoreGive(s_lock);
    return err;
}

unsigned dir_history_unseen_missed(void)
{
    if (s_lock == NULL) {
        return 0;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    unsigned n = dir_hist_unseen_missed(&s_hist);
    xSemaphoreGive(s_lock);
    return n;
}

esp_err_t dir_history_mark_seen(void)
{
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_INVALID_STATE, TAG, "not initialised");
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = dir_hist_mark_seen(&s_hist) ? save_history() : ESP_OK;
    xSemaphoreGive(s_lock);
    return err;
}
