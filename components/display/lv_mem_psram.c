/* LVGL heap (CONFIG_LV_USE_CUSTOM_MALLOC): objects, styles and labels live in PSRAM.
 *
 * With the C library allocator every allocation under CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL
 * (16 KB) lands in internal RAM, i.e. all of LVGL. A screen switch keeps two screens alive
 * until the old one is deleted, which drained internal RAM to a few hundred bytes and broke
 * Wi-Fi, task creation (OTA "Out of memory") and finally the LVGL task itself. The draw
 * buffers are not affected: esp_lvgl_port allocates them with DMA caps itself.
 *
 * Falls back to the default heap when PSRAM is missing or full. */

#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "lvgl.h"

#define PSRAM_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

void lv_mem_init(void)
{
}

void lv_mem_deinit(void)
{
}

lv_mem_pool_t lv_mem_add_pool(void *mem, size_t bytes)
{
    (void)mem;
    (void)bytes;
    return NULL;
}

void lv_mem_remove_pool(lv_mem_pool_t pool)
{
    (void)pool;
}

void *lv_malloc_core(size_t size)
{
    void *p = heap_caps_malloc(size, PSRAM_CAPS);
    return p ? p : heap_caps_malloc(size, MALLOC_CAP_8BIT);
}

void *lv_realloc_core(void *p, size_t new_size)
{
    if (p == NULL || esp_ptr_external_ram(p)) {
        void *q = heap_caps_realloc(p, new_size, PSRAM_CAPS);
        if (q || new_size == 0) {
            return q;
        }
    }
    return heap_caps_realloc(p, new_size, MALLOC_CAP_8BIT);
}

void lv_free_core(void *p)
{
    heap_caps_free(p);
}

void lv_mem_monitor_core(lv_mem_monitor_t *mon_p)
{
    multi_heap_info_t info;
    heap_caps_get_info(&info, PSRAM_CAPS);
    mon_p->total_size = heap_caps_get_total_size(PSRAM_CAPS);
    mon_p->free_size = info.total_free_bytes;
    mon_p->free_biggest_size = info.largest_free_block;
    mon_p->free_cnt = info.free_blocks;
    mon_p->max_used = mon_p->total_size - info.minimum_free_bytes;
    mon_p->used_cnt = info.allocated_blocks;
    mon_p->used_pct = mon_p->total_size ? (uint8_t)(100 - 100 * mon_p->free_size / mon_p->total_size) : 0;
    mon_p->frag_pct = 0;
}

lv_result_t lv_mem_test_core(void)
{
    return heap_caps_check_integrity(PSRAM_CAPS, false) ? LV_RESULT_OK : LV_RESULT_INVALID;
}
