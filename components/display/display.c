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
#include "esp_lcd_panel_commands.h"
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
#include "power.h"
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
#define DRAW_BUF_DIVIDER    20                /* each draw buffer = 1/20 screen (16 lines) */
#define SWALLOW_GAP_US      (250 * 1000)      /* touch "release" = no sample for this long */
#define XPT2046_ADC_MAX     4096
#define TOUCH_REARM_US      (30 * 1000)       /* finger still down but not reported: next look */
/* LVGL task: longest sleep with no LVGL timer due. Anything that needs it sooner wakes it
 * (touch interrupt, lv_timer resume / display invalidation via the resume callback). */
#define LVGL_MAX_SLEEP_MS   2000
/* esp_lvgl_port's periodic lv_tick_inc() timer would wake the chip every 5 ms; LVGL reads its
 * clock from esp_timer instead (lv_tick_set_cb) and the port's timer is slowed to ~17 min. */
#define LVGL_PORT_TICK_MS   1000000

/* ILI9341 sleep (CONFIG_SIPWIRE_DISPLAY_PANEL_SLEEP): SLPIN while the backlight is off. Sleep In
 * stops the DC/DC converter, oscillator and panel scanning; the SPI interface and the frame
 * memory keep working, so a flush that reaches a sleeping panel still lands in GRAM and is
 * shown after SLPOUT. Datasheet timing: 5 ms after SLPIN / SLPOUT before the next command,
 * SLPIN no sooner than 120 ms after SLPOUT; the panel takes up to 120 ms after SLPOUT to come
 * up, so the backlight waits that long (no start-up flash). */
#if defined(CONFIG_SIPWIRE_DISPLAY_PANEL_SLEEP) && !CONFIG_SIPWIRE_QEMU
#define PANEL_SLEEP         1
#else
#define PANEL_SLEEP         0
#endif
#define PANEL_CMD_WAIT_MS   5
#define PANEL_SLPOUT_US     (120 * 1000)
#define BL_OFF_LVGL_WAIT_MS 20                /* bl_tick: LVGL busy longer -> try at the next tick */

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
    esp_timer_handle_t bl_tick;         /* runs only while the backlight is on or dimmed */
    esp_timer_handle_t bl_first;
    esp_timer_handle_t bl_ready;        /* lights the backlight once the panel is out of sleep */
    int8_t lit_posted;                  /* last DISPLAY_EVENT_BACKLIGHT value (-1: none yet) */

    /* panel sleep; changed only with the LVGL lock and then bl_lock held */
    esp_lcd_panel_io_handle_t panel_io; /* NULL: no panel sleep (option off, QEMU) */
    bool panel_asleep;
    int64_t panel_slpout_us;            /* last SLPOUT */
    int64_t panel_ready_us;             /* backlight may come on from here (guarded by bl_lock) */
    bool first_flushed;             /* touched only from the LVGL task */
    bool first_done;

    portMUX_TYPE cal_lock;
    touch_cal_t cal;                /* guarded by cal_lock */
    bool swallow;                   /* first touch while backlight off: eat until release */
    int64_t swallow_last_us;        /* touched only from the LVGL task */

    /* touch interrupt (level, also the light-sleep wake source) */
    gpio_num_t touch_irq;
    esp_timer_handle_t touch_rearm;
    bool touch_raw;                 /* last read saw the panel touched (before swallow / hook) */

    /* raw touch hook (calibration); all of this is touched only with the LVGL lock held */
    esp_lcd_touch_handle_t tp;
    display_touch_raw_cb_t raw_cb;
    void *raw_ctx;
    bool raw_down;                  /* hook was told "pressed" and not yet "released" */
    bool raw_have;                  /* process_coordinates saw a sample in this read */
    int32_t raw_x, raw_y;
} s = {
    .cal_lock = portMUX_INITIALIZER_UNLOCKED,
    .touch_irq = GPIO_NUM_NC,
    .lit_posted = -1,
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

/* Caller holds bl_lock. DISPLAY_EVENT_BACKLIGHT on every lit / dark change (status LED: dark
 * at night; ui_bridge: no clock refresh while dark). A post the event queue refused is retried
 * by the next bl_apply_locked() or bl_tick_cb. */
static void bl_post_lit_locked(void)
{
    bool lit = s.bl_state != BL_OFF;
    if (s.lit_posted != (int8_t)lit &&
        esp_event_post(SIPWIRE_DISPLAY_EVENT, DISPLAY_EVENT_BACKLIGHT, &lit, sizeof(lit), 0) == ESP_OK) {
        s.lit_posted = (int8_t)lit;
    }
}

/* Caller holds bl_lock. Every backlight state change ends here, so this is also where the
 * CPU frequency hold follows the screen: full speed while lit or dimmed (LVGL renders,
 * touch / keys are answered), DFS minimum once the backlight is off. BL_INIT (boot) counts
 * as lit. Until PANEL_SLPOUT_US after a panel wake-up the PWM stays at 0; bl_ready lights it. */
static void bl_apply_locked(void)
{
    bool lit = s.bl_state != BL_OFF;
    power_hold(POWER_HOLD_DISPLAY, lit);
    /* the dim/off check has nothing to do while the backlight is off: no 2 Hz wake-ups then */
    if (s.bl_tick) {
        if (lit) {
            esp_timer_start_periodic(s.bl_tick, BL_TICK_US);    /* ESP_ERR_INVALID_STATE if running */
        } else {
            esp_timer_stop(s.bl_tick);
        }
    }
    bl_post_lit_locked();
    if (lit && s.bl_ready) {
        int64_t wait_us = s.panel_ready_us - esp_timer_get_time();
        if (wait_us > 0) {
            bl_set_duty(0);
            if (!esp_timer_is_active(s.bl_ready)) {
                esp_timer_start_once(s.bl_ready, (uint64_t)wait_us);
            }
            return;
        }
    }
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

static void delay_ms(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms) + 1);  /* +1: at least ms, whatever the tick phase */
}

/* Caller holds the LVGL lock, then bl_lock. Sleeps the panel while the backlight is off and
 * wakes it otherwise. The LVGL lock keeps the LVGL task out of lv_timer_handler, so no flush
 * starts meanwhile (LVGL also waits for its last flush before leaving a refresh), and
 * esp_lcd's tx_param first waits for colour transfers still queued: a command never lands in
 * the middle of pixel data. esp_lcd_panel_disp_sleep() is not used: the ILI9341 driver
 * blocks 120 ms after both commands, which would hold the LVGL lock (and, on the way down, the
 * esp_timer task) that long. The 5 ms command gaps are waited here with the LVGL lock held;
 * the 120 ms start-up only delays the backlight (panel_ready_us), LVGL may draw meanwhile. */
static void panel_sync_locked(void)
{
    if (s.panel_io == NULL) {
        return;
    }
    bool sleep = s.bl_state == BL_OFF;
    if (sleep == s.panel_asleep) {
        return;
    }
    esp_err_t err;
    if (sleep) {
        int64_t early_us = s.panel_slpout_us + PANEL_SLPOUT_US - esp_timer_get_time();
        if (early_us > 0) {
            delay_ms((uint32_t)(early_us / 1000));      /* SLPIN >= 120 ms after SLPOUT */
        }
        err = esp_lcd_panel_io_tx_param(s.panel_io, LCD_CMD_SLPIN, NULL, 0);
        delay_ms(PANEL_CMD_WAIT_MS);
        if (err == ESP_OK) {
            s.panel_asleep = true;
        }
    } else {
        err = esp_lcd_panel_io_tx_param(s.panel_io, LCD_CMD_SLPOUT, NULL, 0);
        s.panel_slpout_us = esp_timer_get_time();
        s.panel_ready_us = s.panel_slpout_us + PANEL_SLPOUT_US;
        delay_ms(PANEL_CMD_WAIT_MS);
        s.panel_asleep = false;         /* also on error: the backlight must come back anyway */
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "panel %s: %s", sleep ? "SLPIN" : "SLPOUT", esp_err_to_name(err));
    } else {
        ESP_LOGD(TAG, "panel %s", sleep ? "asleep" : "awake");
    }
}

/* Caller holds the LVGL lock (lvgl_held) and then bl_lock: apply a new bl_state. The panel
 * sleeps after the backlight is dark and wakes before the backlight comes back. Without the
 * LVGL lock the panel is left as it is (only bl_tick_cb going dark could get here like that,
 * and it does not). */
static void bl_update_locked(bool lvgl_held)
{
    if (s.bl_state == BL_OFF) {
        bl_apply_locked();
        if (lvgl_held) {
            panel_sync_locked();
        }
    } else {
        if (lvgl_held) {
            panel_sync_locked();
        }
        bl_apply_locked();
    }
}

/* Takes bl_lock and, while the backlight is off (the panel is asleep and has to wake), the
 * LVGL lock before it. Lock order everywhere: LVGL lock, then bl_lock; the touch path calls
 * display_wake() from the LVGL task with the (recursive) LVGL lock already held. Returns
 * whether the LVGL lock was taken; hand that to bl_give(). */
static bool bl_take(void)
{
    bool lvgl_held = false;
    for (;;) {
        xSemaphoreTake(s.bl_lock, portMAX_DELAY);
        if (lvgl_held || s.panel_io == NULL || s.bl_state != BL_OFF) {
            return lvgl_held;
        }
        xSemaphoreGive(s.bl_lock);
        lvgl_port_lock(0);
        lvgl_held = true;
    }
}

static void bl_give(bool lvgl_held)
{
    xSemaphoreGive(s.bl_lock);
    if (lvgl_held) {
        lvgl_port_unlock();
    }
}

/* One-shot, PANEL_SLPOUT_US after a panel wake-up: light the backlight. */
static void bl_ready_cb(void *arg)
{
    (void)arg;
    xSemaphoreTake(s.bl_lock, portMAX_DELAY);
    bl_apply_locked();
    xSemaphoreGive(s.bl_lock);
}

static esp_err_t bl_init(void)
{
    s.bl_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s.bl_lock, ESP_ERR_NO_MEM, TAG, "bl mutex");
    s.bl_present = board_cfg()->lcd.bl != GPIO_NUM_NC;
    power_hold(POWER_HOLD_DISPLAY, true);   /* boot: first frames render at full speed */
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
    bool lvgl_held = bl_take();
    s.bl_pct = pct;
    s.last_activity_us = esp_timer_get_time();
    if (s.bl_state != BL_INIT) {
        s.bl_state = BL_ON;
        bl_update_locked(lvgl_held);
    }
    bl_give(lvgl_held);
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
    bool lvgl_held = bl_take();
    if (s.bl_state == BL_DIM || s.bl_state == BL_OFF) {
        s.bl_state = BL_ON;
        bl_update_locked(lvgl_held);
    }
    bl_give(lvgl_held);
}

bool display_is_lit(void)
{
    return s.bl_state != BL_OFF;
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

    /* Going dark sleeps the panel: LVGL lock first (lock order). Never wait long for it in the
     * esp_timer task; while LVGL is busy the backlight goes off at a later tick instead. */
    bool lvgl_held = false;
    if (idle >= 2 * timeout_us && s.panel_io != NULL) {
        if (!lvgl_port_lock(BL_OFF_LVGL_WAIT_MS)) {
            return;
        }
        lvgl_held = true;
    }
    xSemaphoreTake(s.bl_lock, portMAX_DELAY);
    bl_post_lit_locked();               /* retry a post the event queue refused */
    bl_state_t next = s.bl_state;
    if (idle >= 2 * timeout_us) {
        next = BL_OFF;
    } else if (idle >= timeout_us && s.bl_state == BL_ON) {
        next = BL_DIM;
    }
    if ((next == BL_DIM || next == BL_OFF) && next != s.bl_state) {
        s.bl_state = next;
        bl_update_locked(lvgl_held);
        ESP_LOGI(TAG, "backlight %s%s", next == BL_DIM ? "dim" : "off", s.panel_asleep ? ", panel asleep" : "");
    }
    bl_give(lvgl_held);
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
    settings_touch_cal_t cal;
    uint8_t backlight;
    uint16_t timeout_s;
    SETTINGS_GET(touch_cal, &cal);
    SETTINGS_GET(backlight, &backlight);
    SETTINGS_GET(backlight_timeout_s, &timeout_s);

    portENTER_CRITICAL(&s.cal_lock);
    s.cal.valid = cal.valid;
    s.cal.x_min = cal.x_min;
    s.cal.x_max = cal.x_max;
    s.cal.y_min = cal.y_min;
    s.cal.y_max = cal.y_max;
    portEXIT_CRITICAL(&s.cal_lock);

    bool lvgl_held = bl_take();
    s.bl_pct = backlight > 100 ? 100 : backlight;
    s.bl_timeout_s = timeout_s;
    if (timeout_s == 0 && (s.bl_state == BL_DIM || s.bl_state == BL_OFF)) {
        s.bl_state = BL_ON;         /* timeout disabled: leave dim/off */
    }
    bl_update_locked(lvgl_held);
    bl_give(lvgl_held);
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
    s.touch_raw = true;
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

/* PENIRQ, level low: wakes the LVGL task (and the chip from light sleep, same level). Masked
 * here until a read finds the panel released, so a held finger does not keep firing it;
 * LVGL's own read timer samples while it reports a press. */
static void touch_isr(void *arg)
{
    gpio_intr_disable(s.touch_irq);
    lvgl_port_task_wake(LVGL_PORT_EVENT_TOUCH, arg);
}

static void touch_rearm_cb(void *arg)
{
    (void)arg;
    gpio_intr_enable(s.touch_irq);
}

/* LVGL task. 0: unmask now (a touch already down fires at once), else after delay_us. */
static void touch_irq_arm(uint32_t delay_us)
{
    if (s.touch_irq == GPIO_NUM_NC) {
        return;
    }
    if (delay_us == 0 || s.touch_rearm == NULL) {
        gpio_intr_enable(s.touch_irq);
    } else {
        esp_timer_start_once(s.touch_rearm, delay_us);          /* ESP_ERR_INVALID_STATE if armed */
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
    s.touch_raw = false;
    if (esp_lcd_touch_read_data(s.tp) != ESP_OK ||
        esp_lcd_touch_get_data(s.tp, pt, &cnt, 1) != ESP_OK) {
        cnt = 0;
    }
    if (cnt == 0) {
        /* LVGL sees no press, so its read timer stops: the interrupt takes over again. While
         * the finger is still down (swallowed wake-up touch, calibration hook) look again in
         * TOUCH_REARM_US instead of at once, or the level interrupt would re-fire per read. */
        touch_irq_arm(s.touch_raw ? TOUCH_REARM_US : 0);
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
    if (b->touch.irq != GPIO_NUM_NC) {
        /* esp_lcd_touch armed a falling-edge interrupt, which cannot wake the chip from light
         * sleep. Swap in a level-low interrupt (wake source too) with our own masking ISR. */
        const esp_timer_create_args_t ra = { .callback = touch_rearm_cb, .name = "touch_rearm" };
        ESP_RETURN_ON_ERROR(esp_timer_create(&ra, &s.touch_rearm), TAG, "touch timer");
        gpio_intr_disable(b->touch.irq);
        ESP_RETURN_ON_ERROR(gpio_isr_handler_remove(b->touch.irq), TAG, "touch isr remove");
        s.touch_irq = b->touch.irq;
        ESP_RETURN_ON_ERROR(gpio_isr_handler_add(b->touch.irq, touch_isr, indev), TAG, "touch isr");
        ESP_RETURN_ON_ERROR(gpio_wakeup_enable(b->touch.irq, GPIO_INTR_LOW_LEVEL), TAG, "touch wake");
        ESP_RETURN_ON_ERROR(gpio_intr_enable(b->touch.irq), TAG, "touch intr");
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

static uint32_t lvgl_tick_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* Any task, under the LVGL lock: a timer was created / resumed / made ready (incl. the display
 * refresh timer on invalidation): end the LVGL task's sleep so it runs now. */
static void lvgl_resume_cb(void *data)
{
    (void)data;
    lvgl_port_task_wake(LVGL_PORT_EVENT_USER, NULL);
}

/* LVGL port (task + tick), once. Idle wake-ups: none from LVGL itself except due lv_timers and
 * one per LVGL_MAX_SLEEP_MS. */
static esp_err_t lvgl_start(void)
{
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_affinity = 0;
    port_cfg.task_stack = 8192;
    port_cfg.task_max_sleep_ms = LVGL_MAX_SLEEP_MS;
    port_cfg.timer_period_ms = LVGL_PORT_TICK_MS;
    ESP_RETURN_ON_ERROR(lvgl_port_init(&port_cfg), TAG, "lvgl_port_init");
    lvgl_port_lock(0);
    lv_tick_set_cb(lvgl_tick_ms);
    lv_timer_handler_set_resume_cb(lvgl_resume_cb, NULL);
    lvgl_port_unlock();
    return ESP_OK;
}

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
    ESP_RETURN_ON_ERROR(lvgl_start(), TAG, "lvgl");

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
    ESP_RETURN_ON_ERROR(lvgl_start(), TAG, "lvgl");

    /* Draw buffers: 1/20 screen each (240 x 16 px = 7.5 KB), double buffered, internal DMA
     * RAM: LVGL renders into one while the other is sent. One flush is a single SPI
     * transaction, so it must fit max_transfer_sz set in board_init() (h_res * 40 pixels *
     * 2 bytes = 19.2 KB; 1/10 screen = 15 KB fitted too). The clamp keeps that true for other
     * panel sizes. */
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
    const esp_timer_create_args_t ready_args = { .callback = bl_ready_cb, .name = "bl_ready" };
    ESP_RETURN_ON_ERROR(esp_timer_create(&first_args, &s.bl_first), TAG, "timer");
    ESP_RETURN_ON_ERROR(esp_timer_create(&tick_args, &s.bl_tick), TAG, "timer");
    ESP_RETURN_ON_ERROR(esp_timer_create(&ready_args, &s.bl_ready), TAG, "timer");
    xSemaphoreTake(s.bl_lock, portMAX_DELAY);
    bl_apply_locked();                  /* starts bl_tick unless the backlight is off */
#if PANEL_SLEEP
    s.panel_io = io;                    /* from now on the panel sleeps while the backlight is off */
#endif
    xSemaphoreGive(s.bl_lock);
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
