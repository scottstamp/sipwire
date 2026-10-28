#include "keypad.h"

#include <stdbool.h>
#include <stdint.h>
#include "board.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "keypad_scan.h"
#include "sipwire_events.h"

static const char *TAG = "keypad";

#define SCAN_PERIOD_MS   10
#define SETTLE_US        10
#define TASK_STACK       3072
#define TASK_PRIO        2
#define TASK_CORE        0

static const board_cfg_t *s_cfg;
static TaskHandle_t s_task;
static keypad_scan_state_t s_scan;

static esp_err_t check_pins(const board_cfg_t *cfg)
{
    for (int r = 0; r < KEYPAD_ROWS; r++) {
        gpio_num_t pin = cfg->keypad.rows[r];
        if (pin == GPIO_NUM_NC) {
            ESP_LOGE(TAG, "row %d is GPIO_NUM_NC; set CONFIG_SIPWIRE_KP_ROW%d", r, r);
            return ESP_ERR_INVALID_ARG;
        }
        ESP_RETURN_ON_FALSE(GPIO_IS_VALID_GPIO(pin), ESP_ERR_INVALID_ARG, TAG,
                            "row %d: gpio %d invalid", r, pin);
    }
    for (int c = 0; c < KEYPAD_COLS; c++) {
        gpio_num_t pin = cfg->keypad.cols[c];
        if (pin == GPIO_NUM_NC) {
            ESP_LOGE(TAG, "col %d is GPIO_NUM_NC; set CONFIG_SIPWIRE_KP_COL%d", c, c);
            return ESP_ERR_INVALID_ARG;
        }
        ESP_RETURN_ON_FALSE(GPIO_IS_VALID_GPIO(pin), ESP_ERR_INVALID_ARG, TAG,
                            "col %d: gpio %d invalid", c, pin);
    }
    /* A pin used twice would short a row to a column. */
    for (int r = 0; r < KEYPAD_ROWS; r++) {
        for (int c = 0; c < KEYPAD_COLS; c++) {
            ESP_RETURN_ON_FALSE(cfg->keypad.rows[r] != cfg->keypad.cols[c],
                                ESP_ERR_INVALID_ARG, TAG,
                                "gpio %d used for row %d and col %d",
                                cfg->keypad.rows[r], r, c);
        }
    }
    return ESP_OK;
}

static esp_err_t configure_gpios(const board_cfg_t *cfg)
{
    /* Columns: inputs with pull-ups; a pressed key pulls its column low. */
    for (int c = 0; c < KEYPAD_COLS; c++) {
        const gpio_config_t io = {
            .pin_bit_mask = 1ULL << cfg->keypad.cols[c],
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "col %d", c);
    }
    /* Rows: open-drain. Level 0 sinks the row; level 1 releases it to high-Z,
     * so an idle row never drives anything and two keys can't short outputs. */
    for (int r = 0; r < KEYPAD_ROWS; r++) {
        const gpio_config_t io = {
            .pin_bit_mask = 1ULL << cfg->keypad.rows[r],
            .mode = GPIO_MODE_INPUT_OUTPUT_OD,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "row %d", r);
        ESP_RETURN_ON_ERROR(gpio_set_level(cfg->keypad.rows[r], 1), TAG, "row %d idle", r);
    }
    return ESP_OK;
}

/* Drive each row low in turn and sample the columns. Returns a raw mask, bit
 * index = row * KEYPAD_COLS + col. */
static uint16_t scan_matrix(const board_cfg_t *cfg)
{
    uint16_t mask = 0;
    for (int r = 0; r < KEYPAD_ROWS; r++) {
        const gpio_num_t row = cfg->keypad.rows[r];
        gpio_set_level(row, 0);
        esp_rom_delay_us(SETTLE_US);
        for (int c = 0; c < KEYPAD_COLS; c++) {
            if (gpio_get_level(cfg->keypad.cols[c]) == 0) {
                mask |= (uint16_t)(1u << (r * KEYPAD_COLS + c));
            }
        }
        gpio_set_level(row, 1);
    }
    return mask;
}

static void post_event(const keypad_scan_event_t *e)
{
    key_event_id_t id;
    switch (e->type) {
    case KEYPAD_SCAN_PRESS:   id = KEY_EVENT_PRESS;   break;
    case KEYPAD_SCAN_RELEASE: id = KEY_EVENT_RELEASE; break;
    case KEYPAD_SCAN_LONG:    id = KEY_EVENT_LONG;    break;
    case KEYPAD_SCAN_REPEAT:  id = KEY_EVENT_REPEAT;  break;
    default:                  return;
    }
    key_event_t kev = { .key = s_cfg->keypad.map[e->key] };
    esp_err_t err = esp_event_post(SIPWIRE_KEY_EVENT, id, &kev, sizeof(kev), 0);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "key '%c' event %d dropped: %s", kev.key, (int)id, esp_err_to_name(err));
    }
}

static void scan_task(void *arg)
{
    (void)arg;
    TickType_t last = xTaskGetTickCount();
    for (;;) {
        const uint16_t raw = scan_matrix(s_cfg);
        const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        keypad_scan_event_t ev[KEYPAD_MAX_EVENTS];
        const size_t n = keypad_scan_step(&s_scan, raw, now, ev, KEYPAD_MAX_EVENTS);
        for (size_t i = 0; i < n; i++) {
            post_event(&ev[i]);
        }
        vTaskDelayUntil(&last, pdMS_TO_TICKS(SCAN_PERIOD_MS));
    }
}

esp_err_t keypad_init(void)
{
    ESP_RETURN_ON_FALSE(s_task == NULL, ESP_ERR_INVALID_STATE, TAG, "already started");

    const board_cfg_t *cfg = board_cfg();
    ESP_RETURN_ON_ERROR(check_pins(cfg), TAG, "pin check");
    ESP_RETURN_ON_FALSE(cfg->keypad.map[0] != '\0', ESP_ERR_INVALID_STATE, TAG,
                        "key map empty; board_init() must run first");

    ESP_RETURN_ON_ERROR(configure_gpios(cfg), TAG, "gpio config");

    s_cfg = cfg;
    keypad_scan_reset(&s_scan);
    BaseType_t ok = xTaskCreatePinnedToCore(scan_task, TAG, TASK_STACK, NULL,
                                            TASK_PRIO, &s_task, TASK_CORE);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "scan task");

    ESP_LOGI(TAG, "polling %d rows x %d cols every %d ms", KEYPAD_ROWS, KEYPAD_COLS, SCAN_PERIOD_MS);
    return ESP_OK;
}
