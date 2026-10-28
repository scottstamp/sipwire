#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "lvgl.h"
#include "nvs_flash.h"

#include "audio.h"
#include "board.h"
#include "console.h"
#include "display.h"
#include "net.h"
#include "phone.h"
#include "settings.h"
#include "ui_bridge.h"

static const char *TAG = "sipwire";

static esp_err_t nvs_setup(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "nvs erase");
        err = nvs_flash_init();
    }
    return err;
}

void app_main(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    ESP_LOGI(TAG, "sipwire %s (idf %s, LVGL %d.%d.%d)", app->version, app->idf_ver, LVGL_VERSION_MAJOR,
             LVGL_VERSION_MINOR, LVGL_VERSION_PATCH);

    ESP_ERROR_CHECK(nvs_setup());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(board_init());
    ESP_ERROR_CHECK(settings_init());
    ESP_ERROR_CHECK(net_init());
    esp_err_t err = audio_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "audio_init: %s (calls will be silent)", esp_err_to_name(err));
    }

    lv_display_t *disp = NULL;
    ESP_ERROR_CHECK(display_init(&disp));
    ESP_ERROR_CHECK(ui_bridge_init(disp));
    ESP_ERROR_CHECK(phone_init());

    ESP_ERROR_CHECK(console_start());
}
