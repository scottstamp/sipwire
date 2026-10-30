#include "status_led.h"

#include "board.h"
#include "driver/rmt_encoder.h"
#include "driver/rmt_tx.h"
#include <stdatomic.h>
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#include "sipwire_events.h"
#include "status_led_pattern.h"

static const char *TAG = "status_led";

#define RMT_RES_HZ   (10 * 1000 * 1000)  /* 0.1 us ticks */
#define TICK_MS      20
#define MAX_LEVEL    40                  /* WS2812 is blinding at full scale */

static struct {
    rmt_channel_handle_t chan;
    rmt_encoder_handle_t enc;
    esp_timer_handle_t timer;
    sl_state_t st;                       /* written by event handler, read by timer */
    sl_rgb_t shown;
    bool shown_valid;
    atomic_bool dark;                    /* backlight off: LED off except for an incoming call */
} s = {
    .st = { .booting = true, .net = SL_NET_DOWN, .reg = SL_REG_NONE, .call = SL_CALL_IDLE },
};

static void show(sl_rgb_t c)
{
    if (s.shown_valid && c.r == s.shown.r && c.g == s.shown.g && c.b == s.shown.b) {
        return;
    }
    static uint8_t grb[3];               /* must stay valid until the transfer is done */
    grb[0] = c.g;
    grb[1] = c.r;
    grb[2] = c.b;
    /* The channel is enabled only around a frame: an enabled RMT TX channel holds a
     * CPU_FREQ_MAX PM lock, which kept DFS at 240 MHz all the time. A frame is 24 bits of
     * 1.2 us, so waiting for it here (esp_timer task) costs ~30 us per colour change. */
    const rmt_transmit_config_t tx = { .loop_count = 0 };
    if (rmt_enable(s.chan) != ESP_OK) {
        return;                          /* retry next tick */
    }
    if (rmt_transmit(s.chan, s.enc, grb, sizeof(grb), &tx) == ESP_OK &&
        rmt_tx_wait_all_done(s.chan, 10) == ESP_OK) {
        s.shown = c;
        s.shown_valid = true;
    }
    rmt_disable(s.chan);
}

static bool want_running(void)
{
    return !atomic_load(&s.dark) || s.st.call == SL_CALL_INCOMING;
}

/* (Re)start the pattern timer when the LED should show something. Called after the state
 * it depends on changed; a timer that is already running is left alone. */
static void wake(void)
{
    if (s.timer && want_running()) {
        esp_timer_start_periodic(s.timer, TICK_MS * 1000);     /* ESP_ERR_INVALID_STATE if running */
    }
}

static void tick_cb(void *arg)
{
    (void)arg;
    if (!want_running()) {
        /* screen off and no incoming call: LED off, and no 50 Hz wakeups while dark */
        show((sl_rgb_t){ 0, 0, 0 });
        esp_timer_stop(s.timer);
        wake();                          /* lit again meanwhile: keep going */
        return;
    }
    sl_state_t st = s.st;
    show(sl_pattern(&st, (uint32_t)(esp_timer_get_time() / 1000), MAX_LEVEL));
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == SIPWIRE_NET_EVENT) {
        s.st.booting = false;
        if (id == NET_EVENT_GOT_IP) {
            s.st.net = SL_NET_UP;
        } else if (id == NET_EVENT_DISCONNECTED) {
            s.st.net = SL_NET_DOWN;
        }
    } else if (base == SIPWIRE_SIP_EVENT && id == SIP_EVENT_MWI && data) {
        const sip_event_mwi_t *m = data;
        s.st.mwi = m->known && m->waiting;
    } else if (base == SIPWIRE_SIP_EVENT && id == SIP_EVENT_REG_STATE && data) {
        switch (((const sip_event_reg_t *)data)->state) {
        case SIP_REG_REGISTERING: s.st.reg = SL_REG_REGISTERING; break;
        case SIP_REG_REGISTERED:  s.st.reg = SL_REG_REGISTERED; break;
        case SIP_REG_FAILED:      s.st.reg = SL_REG_FAILED; break;
        default:                  s.st.reg = SL_REG_NONE; break;
        }
    } else if (base == SIPWIRE_CALL_EVENT && id == CALL_EVENT_STATE && data) {
        switch (((const call_event_state_t *)data)->state) {
        case CALL_DIALING:
        case CALL_RINGING_OUT: s.st.call = SL_CALL_OUTGOING; break;
        case CALL_RINGING_IN:  s.st.call = SL_CALL_INCOMING; break;
        case CALL_ACTIVE:      s.st.call = SL_CALL_ACTIVE; break;
        case CALL_HELD:        s.st.call = SL_CALL_HELD; break;
        default:               s.st.call = SL_CALL_IDLE; break;
        }
    } else if (base == SIPWIRE_DISPLAY_EVENT && id == DISPLAY_EVENT_BACKLIGHT && data) {
        atomic_store(&s.dark, !*(const bool *)data);
    }
    wake();
}

esp_err_t status_led_init(void)
{
    gpio_num_t pin = board_cfg()->status_led;
#if CONFIG_SIPWIRE_QEMU
    pin = GPIO_NUM_NC;                   /* no RMT in QEMU */
#endif
    if (pin == GPIO_NUM_NC) {
        ESP_LOGI(TAG, "no status LED");
        return ESP_OK;
    }

    const rmt_tx_channel_config_t chan_cfg = {
        .gpio_num = pin,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = RMT_RES_HZ,
        .mem_block_symbols = 48,
        .trans_queue_depth = 2,
    };
    ESP_RETURN_ON_ERROR(rmt_new_tx_channel(&chan_cfg, &s.chan), TAG, "rmt channel");

    /* WS2812: T0H 0.3 us / T0L 0.9 us, T1H 0.9 us / T1L 0.3 us, MSB first. Frames
     * are >= 20 ms apart, far longer than the 280 us reset/latch time. */
    const rmt_bytes_encoder_config_t enc_cfg = {
        .bit0 = { .level0 = 1, .duration0 = 3, .level1 = 0, .duration1 = 9 },
        .bit1 = { .level0 = 1, .duration0 = 9, .level1 = 0, .duration1 = 3 },
        .flags.msb_first = 1,
    };
    ESP_RETURN_ON_ERROR(rmt_new_bytes_encoder(&enc_cfg, &s.enc), TAG, "rmt encoder");

    ESP_RETURN_ON_ERROR(esp_event_handler_register(SIPWIRE_NET_EVENT, ESP_EVENT_ANY_ID, on_event, NULL),
                        TAG, "net events");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(SIPWIRE_SIP_EVENT, ESP_EVENT_ANY_ID, on_event, NULL),
                        TAG, "sip events");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(SIPWIRE_CALL_EVENT, ESP_EVENT_ANY_ID, on_event, NULL),
                        TAG, "call events");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(SIPWIRE_DISPLAY_EVENT, ESP_EVENT_ANY_ID, on_event, NULL),
                        TAG, "display events");

    const esp_timer_create_args_t targs = { .callback = tick_cb, .name = "status_led" };
    ESP_RETURN_ON_ERROR(esp_timer_create(&targs, &s.timer), TAG, "timer");
    ESP_RETURN_ON_ERROR(esp_timer_start_periodic(s.timer, TICK_MS * 1000), TAG, "timer start");
    ESP_LOGI(TAG, "WS2812 on GPIO %d", pin);
    return ESP_OK;
}
