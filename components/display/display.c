#include "display.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_lcd_ili9341.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_touch_xpt2046.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"

#include "board.h"
#include "settings.h"
#include "sipwire_events.h"

static const char *TAG = "display";

/* ---- tunables ---- */
#define BL_LEDC_MODE        LEDC_LOW_SPEED_MODE
#define BL_LEDC_TIMER       LEDC_TIMER_0
#define BL_LEDC_CHANNEL     LEDC_CHANNEL_0
#define BL_LEDC_RES         LEDC_TIMER_10_BIT
#define BL_LEDC_MAX_DUTY    ((1u << 10) - 1)
#define BL_LEDC_FREQ_HZ     5000
#define BL_DIM_PCT          10                /* brightness while dimmed */
#define BL_TICK_US          (500 * 1000)
#define BL_FIRST_FRAME_US   (100 * 1000)      /* let the last SPI transfers finish */
#define DRAW_BUF_DIVIDER    10                /* each draw buffer = 1/10 screen */
#define SWALLOW_GAP_US      (250 * 1000)      /* touch "release" = no sample for this long */
#define XPT2046_ADC_MAX     4096

typedef enum {
    BL_INIT = 0,    /* waiting for the first frame; backlight stays off */
    BL_ON,
    BL_DIM,
    BL_OFF,
} bl_state_t;

typedef struct {
    bool valid;
    int16_t x_min, x_max, y_min, y_max;
} touch_cal_t;

static struct {
    lv_display_t *disp;
    int hres, vres;                 /* logical (LVGL) size, after swap_xy */
    bool bl_present;

    SemaphoreHandle_t bl_lock;
    volatile bl_state_t bl_state;
    uint8_t bl_pct;                 /* user brightness (settings), guarded by bl_lock */
    volatile uint32_t bl_timeout_s; /* 0 = never */
    volatile int64_t last_activity_us;
    esp_timer_handle_t bl_tick;
    esp_timer_handle_t bl_first;
    bool first_flushed;             /* touched only from the LVGL task */
    bool first_done;

    portMUX_TYPE cal_lock;
    touch_cal_t cal;                /* guarded by cal_lock */
    bool swallow;                   /* first touch while backlight off: eat until release */
    int64_t swallow_last_us;        /* touched only from the LVGL task */

    /* raw touch hook (calibration); all of this is touched only with the LVGL lock held */
    esp_lcd_touch_handle_t tp;
    display_touch_raw_cb_t raw_cb;
    void *raw_ctx;
    bool raw_down;                  /* hook was told "pressed" and not yet "released" */
    bool raw_have;                  /* process_coordinates saw a sample in this read */
    int32_t raw_x, raw_y;
} s = {
    .cal_lock = portMUX_INITIALIZER_UNLOCKED,
};

/* ------------------------------------------------------------------ backlight */

static void bl_set_duty(uint32_t pct)
{
    if (!s.bl_present) {
        return;
    }
    if (pct > 100) {
        pct = 100;
    }
    uint32_t duty = (pct * BL_LEDC_MAX_DUTY + 50) / 100;
    ledc_set_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL, duty);
    ledc_update_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL);
}

/* Caller holds bl_lock. */
static void bl_apply_locked(void)
{
    switch (s.bl_state) {
    case BL_ON:
        bl_set_duty(s.bl_pct);
        break;
    case BL_DIM:
        bl_set_duty(s.bl_pct < BL_DIM_PCT ? s.bl_pct : BL_DIM_PCT);
        break;
    case BL_OFF:
    case BL_INIT:
    default:
        bl_set_duty(0);
        break;
    }
}

static esp_err_t bl_init(void)
{
    s.bl_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s.bl_lock, ESP_ERR_NO_MEM, TAG, "bl mutex");
    s.bl_present = board_cfg()->lcd.bl != GPIO_NUM_NC;
    s.last_activity_us = esp_timer_get_time();

    if (s.bl_present) {
        const ledc_timer_config_t timer = {
            .speed_mode = BL_LEDC_MODE,
            .duty_resolution = BL_LEDC_RES,
            .timer_num = BL_LEDC_TIMER,
            .freq_hz = BL_LEDC_FREQ_HZ,
            .clk_cfg = LEDC_AUTO_CLK,
        };
        ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "ledc timer");
        const ledc_channel_config_t ch = {
            .gpio_num = board_cfg()->lcd.bl,
            .speed_mode = BL_LEDC_MODE,
            .channel = BL_LEDC_CHANNEL,
            .timer_sel = BL_LEDC_TIMER,
            .duty = 0,
            .hpoint = 0,
            .flags.output_invert = !board_cfg()->lcd.bl_active_high,
        };
        ESP_RETURN_ON_ERROR(ledc_channel_config(&ch), TAG, "ledc channel");
    }
    return ESP_OK;
}

void display_set_backlight(uint8_t pct)
{
    if (s.bl_lock == NULL) {
        return;
    }
    if (pct > 100) {
        pct = 100;
    }
    xSemaphoreTake(s.bl_lock, portMAX_DELAY);
    s.bl_pct = pct;
    s.last_activity_us = esp_timer_get_time();
    if (s.bl_state != BL_INIT) {
        s.bl_state = BL_ON;
        bl_apply_locked();
    }
    xSemaphoreGive(s.bl_lock);
}

void display_wake(void)
{
    if (s.bl_lock == NULL) {
        return;
    }
    s.last_activity_us = esp_timer_get_time();
    bl_state_t st = s.bl_state;
    if (st == BL_ON || st == BL_INIT) {
        return;
    }
    xSemaphoreTake(s.bl_lock, portMAX_DELAY);
    if (s.bl_state == BL_DIM || s.bl_state == BL_OFF) {
        s.bl_state = BL_ON;
        bl_apply_locked();
    }
    xSemaphoreGive(s.bl_lock);
}

/* Periodic idle check: ON -> DIM after the timeout, -> OFF after 2x the timeout. */
static void bl_tick_cb(void *arg)
{
    (void)arg;
    uint32_t timeout_s = s.bl_timeout_s;
    if (timeout_s == 0 || s.bl_state == BL_INIT || s.bl_state == BL_OFF) {
        return;
    }
    int64_t idle = esp_timer_get_time() - s.last_activity_us;
    int64_t timeout_us = (int64_t)timeout_s * 1000000;

    xSemaphoreTake(s.bl_lock, portMAX_DELAY);
    bl_state_t next = s.bl_state;
    if (idle >= 2 * timeout_us) {
        next = BL_OFF;
    } else if (idle >= timeout_us && s.bl_state == BL_ON) {
        next = BL_DIM;
    }
    if ((next == BL_DIM || next == BL_OFF) && next != s.bl_state) {
        s.bl_state = next;
        bl_apply_locked();
        ESP_LOGI(TAG, "backlight %s", next == BL_DIM ? "dim" : "off");
    }
    xSemaphoreGive(s.bl_lock);
}

/* One-shot, shortly after the first complete frame has been flushed. */
static void bl_first_cb(void *arg)
{
    (void)arg;
    xSemaphoreTake(s.bl_lock, portMAX_DELAY);
    if (s.bl_state == BL_INIT) {
        s.bl_state = BL_ON;
        s.last_activity_us = esp_timer_get_time();
        bl_apply_locked();
        ESP_LOGI(TAG, "backlight on (%u%%)", (unsigned)s.bl_pct);
    }
    xSemaphoreGive(s.bl_lock);
}

/* LVGL task context (under the LVGL lock). */
static void flush_finish_cb(lv_event_t *e)
{
    (void)e;
    s.first_flushed = true;
}

static void refr_ready_cb(lv_event_t *e)
{
    (void)e;
    if (s.first_flushed && !s.first_done) {
        s.first_done = true;
        esp_timer_start_once(s.bl_first, BL_FIRST_FRAME_US);
    }
}

/* ------------------------------------------------------------------- settings */

static void apply_settings(void)
{
    settings_t cfg;
    settings_get(&cfg);

    portENTER_CRITICAL(&s.cal_lock);
    s.cal.valid = cfg.touch_cal.valid;
    s.cal.x_min = cfg.touch_cal.x_min;
    s.cal.x_max = cfg.touch_cal.x_max;
    s.cal.y_min = cfg.touch_cal.y_min;
    s.cal.y_max = cfg.touch_cal.y_max;
    portEXIT_CRITICAL(&s.cal_lock);

    xSemaphoreTake(s.bl_lock, portMAX_DELAY);
    s.bl_pct = cfg.backlight > 100 ? 100 : cfg.backlight;
    s.bl_timeout_s = cfg.backlight_timeout_s;
    if (cfg.backlight_timeout_s == 0 && (s.bl_state == BL_DIM || s.bl_state == BL_OFF)) {
        s.bl_state = BL_ON;         /* timeout disabled: leave dim/off */
    }
    bl_apply_locked();
    xSemaphoreGive(s.bl_lock);
}

static void settings_changed_cb(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)id;
    (void)data;
    apply_settings();
}

/* ---------------------------------------------------------------------- touch */

static inline int32_t clamp_i32(int32_t v, int32_t lo, int32_t hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* Raw XPT2046 axes (0..4095) -> screen pixels. All swap/mirror handling is done here
 * (the esp_lcd_touch flags stay clear), using the touch's own board options, since a
 * module's touch axes need not match its panel: mirror acts on the raw axes, then
 * swap_xy exchanges them. With a valid calibration
 * the raw range is mapped directly (direction is encoded by min/max) and mirror is skipped. */
static void touch_to_screen(int32_t rx, int32_t ry, int32_t *sx, int32_t *sy)
{
    const board_cfg_t *b = board_cfg();
    touch_cal_t cal;
    portENTER_CRITICAL(&s.cal_lock);
    cal = s.cal;
    portEXIT_CRITICAL(&s.cal_lock);

    if (cal.valid && cal.x_max != cal.x_min && cal.y_max != cal.y_min) {
        int32_t ax = b->touch.swap_xy ? ry : rx;
        int32_t ay = b->touch.swap_xy ? rx : ry;
        *sx = (ax - cal.x_min) * (s.hres - 1) / (cal.x_max - cal.x_min);
        *sy = (ay - cal.y_min) * (s.vres - 1) / (cal.y_max - cal.y_min);
    } else {
        int32_t nx = rx, ny = ry;
        if (b->touch.mirror_x) {
            nx = XPT2046_ADC_MAX - 1 - nx;
        }
        if (b->touch.mirror_y) {
            ny = XPT2046_ADC_MAX - 1 - ny;
        }
        if (b->touch.swap_xy) {
            int32_t t = nx;
            nx = ny;
            ny = t;
        }
        *sx = nx * s.hres / XPT2046_ADC_MAX;
        *sy = ny * s.vres / XPT2046_ADC_MAX;
    }
    *sx = clamp_i32(*sx, 0, s.hres - 1);
    *sy = clamp_i32(*sy, 0, s.vres - 1);
}

/* Called by esp_lcd_touch for every sample with a touch (never on release). */
static void touch_process_coordinates(esp_lcd_touch_handle_t tp, uint16_t *x, uint16_t *y,
                                      uint16_t *strength, uint8_t *point_num, uint8_t max_point_num)
{
    (void)tp;
    (void)strength;
    (void)max_point_num;
    if (*point_num == 0) {
        return;
    }
    int64_t now = esp_timer_get_time();

    /* A touch that woke a dark screen must not act on whatever is under the finger:
     * swallow samples until none arrived for SWALLOW_GAP_US (= finger lifted). */
    if (s.swallow) {
        if (now - s.swallow_last_us < SWALLOW_GAP_US) {
            s.swallow_last_us = now;
            *point_num = 0;
            return;
        }
        s.swallow = false;
    }
    if (s.bl_state == BL_OFF) {
        s.swallow = true;
        s.swallow_last_us = now;
        display_wake();
        *point_num = 0;
        return;
    }
    display_wake();

    for (uint8_t i = 0; i < *point_num; i++) {
        int32_t rx = x[i], ry = y[i];
#ifdef CONFIG_XPT2046_CONVERT_ADC_TO_COORDS
        /* Driver already scaled to 0..x_max / 0..y_max: undo to get raw ADC counts back. */
        rx = rx * XPT2046_ADC_MAX / tp->config.x_max;
        ry = ry * XPT2046_ADC_MAX / tp->config.y_max;
#endif
        if (s.raw_cb) {
            /* calibration: keep the sample for touch_read_cb, LVGL gets nothing */
            s.raw_x = board_cfg()->touch.swap_xy ? ry : rx;
            s.raw_y = board_cfg()->touch.swap_xy ? rx : ry;
            s.raw_have = true;
            *point_num = 0;
            return;
        }
        int32_t sx, sy;
        touch_to_screen(rx, ry, &sx, &sy);
        x[i] = (uint16_t)sx;
        y[i] = (uint16_t)sy;
    }
}

/* Replaces esp_lvgl_port's indev read callback (same job: read the controller, report the first
 * point) so a release can be reported to the raw hook too; process_coordinates only runs on
 * samples with a touch. */
static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    esp_lcd_touch_point_data_t pt[1] = {0};
    uint8_t cnt = 0;

    s.raw_have = false;
    if (esp_lcd_touch_read_data(s.tp) != ESP_OK ||
        esp_lcd_touch_get_data(s.tp, pt, &cnt, 1) != ESP_OK) {
        cnt = 0;
    }

    display_touch_raw_cb_t cb = s.raw_cb;
    if (cb) {
        void *ctx = s.raw_ctx;
        data->state = LV_INDEV_STATE_RELEASED;
        if (s.raw_have) {
            s.raw_down = true;      /* before the call: the hook may clear itself */
            cb(s.raw_x, s.raw_y, true, ctx);
        } else if (s.raw_down) {
            s.raw_down = false;
            cb(0, 0, false, ctx);
        }
        return;
    }
    if (cnt > 0) {
        data->point.x = pt[0].x;
        data->point.y = pt[0].y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

void display_touch_raw_hook(display_touch_raw_cb_t cb, void *ctx)
{
#if CONFIG_SIPWIRE_QEMU
    (void)cb;
    (void)ctx;
#else
    bool was_set = s.raw_cb != NULL;
    s.raw_ctx = ctx;
    s.raw_cb = cb;
    s.raw_down = false;
    s.raw_have = false;
    if (was_set && cb == NULL) {
        /* the finger that ended calibration may still be down: LVGL must not click with it */
        s.swallow = true;
        s.swallow_last_us = esp_timer_get_time();
    }
#endif
}

static esp_err_t touch_init(lv_display_t *disp)
{
    const board_cfg_t *b = board_cfg();

    esp_lcd_panel_io_handle_t tp_io = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg = ESP_LCD_TOUCH_IO_SPI_XPT2046_CONFIG(b->touch.cs);
    io_cfg.pclk_hz = b->touch.spi_hz > 0 ? b->touch.spi_hz : ESP_LCD_TOUCH_SPI_CLOCK_HZ;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)b->lcd.host, &io_cfg, &tp_io),
                        TAG, "touch panel io");

    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = b->lcd.h_res,        /* native touch axes; only used by the driver's own scaling */
        .y_max = b->lcd.v_res,
        .rst_gpio_num = GPIO_NUM_NC,
        .int_gpio_num = b->touch.irq, /* NC -> polling */
        .levels = {
            .reset = 0,
            .interrupt = 0,           /* PENIRQ is active low */
        },
        .flags = {                    /* swap/mirror are done in process_coordinates */
            .swap_xy = 0,
            .mirror_x = 0,
            .mirror_y = 0,
        },
        .process_coordinates = touch_process_coordinates,
    };
    esp_lcd_touch_handle_t tp = NULL;
    ESP_RETURN_ON_ERROR(esp_lcd_touch_new_spi_xpt2046(tp_io, &tp_cfg, &tp), TAG, "xpt2046");

    const lvgl_port_touch_cfg_t port_cfg = {
        .disp = disp,
        .handle = tp,
    };
    lv_indev_t *indev = lvgl_port_add_touch(&port_cfg);
    ESP_RETURN_ON_FALSE(indev != NULL, ESP_FAIL, TAG, "lvgl_port_add_touch");
    s.tp = tp;
    if (lvgl_port_lock(0)) {
        lv_indev_set_read_cb(indev, touch_read_cb);
        lvgl_port_unlock();
    }
    ESP_LOGI(TAG, "touch xpt2046: cs=%d irq=%d %d Hz (%s)", b->touch.cs, b->touch.irq, (int)io_cfg.pclk_hz,
             b->touch.irq == GPIO_NUM_NC ? "polling" : "interrupt");
    return ESP_OK;
}

esp_err_t display_touch_calibrate_start(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

/* ------------------------------------------------------------------------ LCD */

#if CONFIG_SIPWIRE_QEMU
/* QEMU smoke-test build: no SPI panel, LVGL renders into a discarding display. */
static void qemu_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    (void)area;
    (void)px_map;
    lv_display_flush_ready(disp);
}

static esp_err_t display_init_qemu(lv_display_t **out_disp)
{
    const board_cfg_t *b = board_cfg();
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_affinity = 0;
    port_cfg.task_stack = 8192;
    ESP_RETURN_ON_ERROR(lvgl_port_init(&port_cfg), TAG, "lvgl_port_init");

    const size_t buf_size = (size_t)b->lcd.h_res * 40 * sizeof(lv_color16_t);
    void *buf = lv_malloc(buf_size);
    ESP_RETURN_ON_FALSE(buf != NULL, ESP_ERR_NO_MEM, TAG, "draw buffer");
    lvgl_port_lock(0);
    s.disp = lv_display_create(b->lcd.h_res, b->lcd.v_res);
    if (s.disp) {
        lv_display_set_color_format(s.disp, LV_COLOR_FORMAT_RGB565);
        lv_display_set_buffers(s.disp, buf, NULL, buf_size, LV_DISPLAY_RENDER_MODE_PARTIAL);
        lv_display_set_flush_cb(s.disp, qemu_flush_cb);
    }
    lvgl_port_unlock();
    ESP_RETURN_ON_FALSE(s.disp != NULL, ESP_ERR_NO_MEM, TAG, "lv_display_create");
    ESP_LOGW(TAG, "QEMU build: no panel, frames discarded");
    *out_disp = s.disp;
    return ESP_OK;
}
#endif

esp_err_t display_init(lv_display_t **out_disp)
{
    ESP_RETURN_ON_FALSE(out_disp != NULL, ESP_ERR_INVALID_ARG, TAG, "out_disp");
    ESP_RETURN_ON_FALSE(s.disp == NULL, ESP_ERR_INVALID_STATE, TAG, "already initialized");
#if CONFIG_SIPWIRE_QEMU
    return display_init_qemu(out_disp);
#endif

    const board_cfg_t *b = board_cfg();
    s.hres = b->lcd.swap_xy ? b->lcd.v_res : b->lcd.h_res;
    s.vres = b->lcd.swap_xy ? b->lcd.h_res : b->lcd.v_res;

    /* Backlight first, so it is held off while the panel powers up. */
    ESP_RETURN_ON_ERROR(bl_init(), TAG, "backlight");

    /* Panel IO + driver. The SPI bus itself was created by board_init(). */
    const esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = b->lcd.cs,
        .dc_gpio_num = b->lcd.dc,
        .spi_mode = 0,
        .pclk_hz = b->lcd.pclk_hz,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    esp_lcd_panel_io_handle_t io = NULL;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)b->lcd.host, &io_cfg, &io),
                        TAG, "lcd panel io");

    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = b->lcd.rst,
        .rgb_ele_order = b->lcd.bgr ? LCD_RGB_ELEMENT_ORDER_BGR : LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    esp_lcd_panel_handle_t panel = NULL;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_ili9341(io, &panel_cfg, &panel), TAG, "ili9341");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(panel), TAG, "panel reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(panel), TAG, "panel init");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(panel, b->lcd.invert), TAG, "invert");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_swap_xy(panel, b->lcd.swap_xy), TAG, "swap_xy");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(panel, b->lcd.mirror_x, b->lcd.mirror_y), TAG, "mirror");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(panel, true), TAG, "disp on");

    /* LVGL port (task + tick), once. */
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_affinity = 0;
    port_cfg.task_stack = 8192;
    ESP_RETURN_ON_ERROR(lvgl_port_init(&port_cfg), TAG, "lvgl_port_init");

    /* Draw buffers: 1/10 screen each, double buffered, internal DMA RAM. One flush is a
     * single SPI transaction, so it must fit max_transfer_sz set in board_init()
     * (h_res * 40 pixels * 2 bytes). */
    uint32_t buf_px = (uint32_t)s.hres * s.vres / DRAW_BUF_DIVIDER;
    const uint32_t max_px = (uint32_t)b->lcd.h_res * 40;
    if (buf_px > max_px) {
        buf_px = max_px;
    }
    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io,
        .panel_handle = panel,
        .buffer_size = buf_px,
        .double_buffer = true,
        .hres = s.hres,
        .vres = s.vres,
        .monochrome = false,
        .rotation = {
            .swap_xy = b->lcd.swap_xy,
            .mirror_x = b->lcd.mirror_x,
            .mirror_y = b->lcd.mirror_y,
        },
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = {
            .buff_dma = true,
            .buff_spiram = false,
            .swap_bytes = true,         /* SPI sends RGB565 big-endian */
        },
    };
    lv_display_t *disp = lvgl_port_add_disp(&disp_cfg);
    ESP_RETURN_ON_FALSE(disp != NULL, ESP_ERR_NO_MEM, TAG, "lvgl_port_add_disp");
    s.disp = disp;

    /* Settings (backlight level/timeout, touch cal) and live updates. */
    apply_settings();
    ESP_RETURN_ON_ERROR(esp_event_handler_register(SIPWIRE_SETTINGS_EVENT, SETTINGS_EVENT_CHANGED,
                                                   settings_changed_cb, NULL),
                        TAG, "settings handler");

    /* Backlight timers; the backlight switches on after the first complete frame. */
    const esp_timer_create_args_t first_args = { .callback = bl_first_cb, .name = "bl_first" };
    const esp_timer_create_args_t tick_args = { .callback = bl_tick_cb, .name = "bl_tick" };
    ESP_RETURN_ON_ERROR(esp_timer_create(&first_args, &s.bl_first), TAG, "timer");
    ESP_RETURN_ON_ERROR(esp_timer_create(&tick_args, &s.bl_tick), TAG, "timer");
    ESP_RETURN_ON_ERROR(esp_timer_start_periodic(s.bl_tick, BL_TICK_US), TAG, "timer start");
    if (lvgl_port_lock(0)) {
        lv_display_add_event_cb(disp, flush_finish_cb, LV_EVENT_FLUSH_FINISH, NULL);
        lv_display_add_event_cb(disp, refr_ready_cb, LV_EVENT_REFR_READY, NULL);
        lvgl_port_unlock();
    }

    if (b->touch.present) {
        esp_err_t err = touch_init(disp);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "touch init failed (%s); continuing without touch", esp_err_to_name(err));
        }
    } else {
        ESP_LOGI(TAG, "no touch controller");
    }

    ESP_LOGI(TAG, "ili9341 %dx%d (native %dx%d) spi%d %d Hz, bgr=%d invert=%d mirror=%d/%d swap=%d, buf 2x%u px",
             s.hres, s.vres, b->lcd.h_res, b->lcd.v_res, b->lcd.host + 1, b->lcd.pclk_hz, b->lcd.bgr,
             b->lcd.invert, b->lcd.mirror_x, b->lcd.mirror_y, b->lcd.swap_xy, (unsigned)buf_px);
    *out_disp = disp;
    return ESP_OK;
}
