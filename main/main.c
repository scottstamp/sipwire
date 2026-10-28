#include "esp_log.h"
#include "esp_lvgl_port.h"

#include "display.h"
#include "ui.h"

static const char *TAG = "sipwire";

void app_main(void)
{
    ESP_LOGI(TAG, "starting, LVGL %d.%d.%d", LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH);

    lv_display_t *disp = NULL;
    ESP_ERROR_CHECK(display_init(&disp));

    lvgl_port_lock(0);
    ui_init(disp);
    lvgl_port_unlock();
}
