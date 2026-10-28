#include "display.h"

#include "esp_check.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"

static const char *TAG = "display";

#define DRAW_BUF_LINES 40

// TODO: replace with a real panel (esp_lcd_new_panel_* + lvgl_port_add_disp)
static void stub_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    (void)area;
    (void)px_map;
    lv_display_flush_ready(disp);
}

esp_err_t display_init(lv_display_t **out_disp)
{
    const lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_RETURN_ON_ERROR(lvgl_port_init(&port_cfg), TAG, "lvgl_port_init failed");

    lvgl_port_lock(0);

    lv_display_t *disp = lv_display_create(SIPWIRE_LCD_H_RES, SIPWIRE_LCD_V_RES);
    if (disp == NULL) {
        lvgl_port_unlock();
        return ESP_ERR_NO_MEM;
    }

    const size_t buf_size = SIPWIRE_LCD_H_RES * DRAW_BUF_LINES * sizeof(lv_color16_t);
    void *buf = lv_malloc(buf_size);
    if (buf == NULL) {
        lv_display_delete(disp);
        lvgl_port_unlock();
        return ESP_ERR_NO_MEM;
    }

    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(disp, buf, NULL, buf_size, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, stub_flush_cb);

    lvgl_port_unlock();

    ESP_LOGI(TAG, "display ready (%dx%d, stub flush)", SIPWIRE_LCD_H_RES, SIPWIRE_LCD_V_RES);
    *out_disp = disp;
    return ESP_OK;
}
