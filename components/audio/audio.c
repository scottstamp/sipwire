#include "audio.h"

#include <stdatomic.h>
#include <string.h>

#include "board.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "audio_aec.h"
#include "audio_dsp.h"

static const char *TAG = "audio";

/* I2S runs at the API rate, 16 kHz (MEMS mics need BCLK >= ~1 MHz anyway);
 * 8 kHz codecs are resampled in rtp, not here. */
#define AUDIO_I2S_RATE          AUDIO_SAMPLE_RATE
_Static_assert(AUDIO_I2S_RATE == 16000, "I2S clocking below assumes 16 kHz");

#define I2S_FRAME_SAMPLES       AUDIO_FRAME_SAMPLES                     /* 20 ms */
#define I2S_CHUNK_SAMPLES       (I2S_FRAME_SAMPLES / 2)                 /* 10 ms per read/write */
#define I2S_DMA_DESC_NUM        4
#define I2S_IO_TIMEOUT_MS       100
#define RX_WARMUP_FRAMES        5       /* MEMS mics output garbage for ~100 ms after clock start */

#define AUDIO_TASK_PRIO         (configMAX_PRIORITIES - 3)
#define AUDIO_TASK_CORE         1
#define TX_STACK_BYTES          6144    /* 2.5 KB of 16 kHz frame buffers + rtp_pull_frame */
#define RX_STACK_BYTES          6144

#define DC_BLOCK_HZ             20
/* A tone-only stream (no callbacks: ringtone preview, console `ring`) whose tone has stopped
 * or ended is shut down after this long, so I2S and the amp do not stay on while idle. */
#define IDLE_STOP_FRAMES        (2000 / 20)
#define DEFAULT_VOLUME          70
#define ALERT_AMP               2458    /* Q15: single tone peaks at 2 x amp = 0.15 FS, before volume */

#define EVT_TX_DONE             BIT0
#define EVT_RX_DONE             BIT1

typedef enum {
    REQ_NONE,
    REQ_TONE,
    REQ_DTMF,
    REQ_RINGTONE,                   /* index in req_digit */
} tone_req_kind_t;

static struct {
    bool inited;
    const board_cfg_t *cfg;
    i2s_chan_handle_t tx, rx;
    SemaphoreHandle_t api_mtx;      /* serialises start/stop */
    SemaphoreHandle_t cb_mtx;       /* held while a callback runs; guards src/sink/ctx */
    EventGroupHandle_t done;
    atomic_bool running;
    atomic_bool stop;
    atomic_bool idle_stopping;      /* tx_task armed idle_timer; tone requests wait in req_* */
    esp_timer_handle_t idle_timer;
    audio_source_cb_t src;
    audio_sink_cb_t sink;
    void *ctx;
    _Atomic uint8_t volume;
    atomic_bool mic_mute;
    portMUX_TYPE req_mux;
    tone_req_kind_t req_kind;
    audio_tone_t req_tone;
    char req_digit;
    atomic_int alert_req;           /* audio_alert_t + 1, 0 = none; tx_task takes it */
} s = {
    .req_mux = portMUX_INITIALIZER_UNLOCKED,
    .volume = DEFAULT_VOLUME,
};

/* ------------------------------------------------------------------ */
/* amp enable                                                         */
/* ------------------------------------------------------------------ */

static void amp_set(bool on)
{
    gpio_num_t en = s.cfg->spk.en;
    if (en == GPIO_NUM_NC) {
        return;
    }
    gpio_set_level(en, on == s.cfg->spk.en_active_high ? 1 : 0);
}

/* The amp is on only while tx_task runs (after the first silent frame, until the last one);
 * every other path leaves it in shutdown. Driven, never floating:
 * - init: the output latch is set to "off" before the pin becomes an output, and no
 *   gpio_reset_pin() (it enables the pull-up: a glitch towards "on" for active-high SD_MODE).
 *   The internal pull is towards "off" too, for the moment the pin is reconfigured.
 * - light sleep: CONFIG_PM_SLP_DISABLE_GPIO isolates every GPIO in sleep (input and output
 *   off, floating), which would leave SD_MODE to the breakout's pull-up. The pin is taken out
 *   of the sleep switch, so it keeps driving "off" (I2S holds a PM lock while a stream runs,
 *   so the chip never sleeps with the amp on).
 * - reset, boot until audio_init(), a panic: the pin is not driven (reset state). Only an
 *   external pull-down keeps the amp off then (docs/HARDWARE.md). The internal pull-down
 *   could not do it either: ~45 kOhm against e.g. a 1 MOhm pull-up to 5 V leaves ~0.2 V on
 *   SD_MODE, above the MAX98357A's 0.16 V shutdown threshold. */
static esp_err_t amp_init(void)
{
    gpio_num_t en = s.cfg->spk.en;
    if (en == GPIO_NUM_NC) {
        return ESP_OK;
    }
    amp_set(false);                     /* output latch first: no glitch when it starts driving */
    const bool high_on = s.cfg->spk.en_active_high;
    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << en,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = high_on ? GPIO_PULLUP_DISABLE : GPIO_PULLUP_ENABLE,
        .pull_down_en = high_on ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "amp en");
    ESP_RETURN_ON_ERROR(gpio_sleep_sel_dis(en), TAG, "amp en sleep");
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* I2S setup                                                          */
/* ------------------------------------------------------------------ */

static void release_channels(void)
{
    if (s.tx) {
        i2s_del_channel(s.tx);
        s.tx = NULL;
    }
    if (s.rx) {
        i2s_del_channel(s.rx);
        s.rx = NULL;
    }
}

/* I2S queue overflow = a task fell 30 ms behind and the driver dropped a DMA
 * buffer, which shifts the speaker->mic sample offset the AEC relies on. */
static bool on_tx_q_ovf(i2s_chan_handle_t handle, i2s_event_data_t *event, void *user_ctx)
{
    (void)handle;
    (void)event;
    (void)user_ctx;
    audio_aec_xrun(true);
    return false;
}

static bool on_rx_q_ovf(i2s_chan_handle_t handle, i2s_event_data_t *event, void *user_ctx)
{
    (void)handle;
    (void)event;
    (void)user_ctx;
    audio_aec_xrun(false);
    return false;
}

static esp_err_t i2s_setup(void)
{
    const board_cfg_t *c = s.cfg;
    const bool duplex = c->mic.full_duplex;
    const bool have_rx = duplex || c->mic.din != GPIO_NUM_NC;
    esp_err_t ret = ESP_OK;

    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    cc.dma_desc_num = I2S_DMA_DESC_NUM;
    cc.dma_frame_num = I2S_CHUNK_SAMPLES;
    cc.auto_clear_after_cb = true;              /* underrun plays silence, not a looping buffer */

    i2s_std_config_t tx_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_I2S_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = c->spk.bclk,
            .ws = c->spk.ws,
            .dout = c->spk.dout,
            .din = I2S_GPIO_UNUSED,
        },
    };
    i2s_std_config_t rx_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_I2S_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = duplex ? c->spk.bclk : c->mic.bclk,
            .ws = duplex ? c->spk.ws : c->mic.ws,
            .dout = I2S_GPIO_UNUSED,
            .din = c->mic.din,
        },
    };
    rx_cfg.slot_cfg.slot_mask = c->mic.slot_right ? I2S_STD_SLOT_RIGHT : I2S_STD_SLOT_LEFT;

    if (duplex) {
        /* Shared BCLK/WS: both directions must produce the same frame (64 BCLK),
         * so the 16-bit TX data sits in a 32-bit slot. 384 x MCLK keeps the
         * internal slave side (RX, initialised second) at an integer ratio >= 6. */
        tx_cfg.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT;
        tx_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_384;
        rx_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_384;
        ESP_GOTO_ON_ERROR(i2s_new_channel(&cc, &s.tx, &s.rx), err, TAG, "new duplex channel");
        ESP_GOTO_ON_ERROR(i2s_channel_init_std_mode(s.tx, &tx_cfg), err, TAG, "init tx");
        ESP_GOTO_ON_ERROR(i2s_channel_init_std_mode(s.rx, &rx_cfg), err, TAG, "init rx");
    } else {
        ESP_GOTO_ON_ERROR(i2s_new_channel(&cc, &s.tx, NULL), err, TAG, "new tx channel");
        ESP_GOTO_ON_ERROR(i2s_channel_init_std_mode(s.tx, &tx_cfg), err, TAG, "init tx");
        if (have_rx) {
            i2s_chan_config_t rc = cc;
            rc.id = I2S_NUM_1;
            ESP_GOTO_ON_ERROR(i2s_new_channel(&rc, NULL, &s.rx), err, TAG, "new rx channel");
            ESP_GOTO_ON_ERROR(i2s_channel_init_std_mode(s.rx, &rx_cfg), err, TAG, "init rx");
        }
    }

    const i2s_event_callbacks_t tx_cbs = { .on_send_q_ovf = on_tx_q_ovf };
    ESP_GOTO_ON_ERROR(i2s_channel_register_event_callback(s.tx, &tx_cbs, NULL), err, TAG, "tx callbacks");
    if (s.rx) {
        const i2s_event_callbacks_t rx_cbs = { .on_recv_q_ovf = on_rx_q_ovf };
        ESP_GOTO_ON_ERROR(i2s_channel_register_event_callback(s.rx, &rx_cbs, NULL), err, TAG, "rx callbacks");
    }
    return ESP_OK;
err:
    release_channels();
    return ret;
}

/* ------------------------------------------------------------------ */
/* tasks                                                              */
/* ------------------------------------------------------------------ */

static void tone_poll(dsp_tone_t *t)
{
    tone_req_kind_t kind;
    audio_tone_t tone;
    char digit;

    portENTER_CRITICAL(&s.req_mux);
    kind = s.req_kind;
    tone = s.req_tone;
    digit = s.req_digit;
    s.req_kind = REQ_NONE;
    portEXIT_CRITICAL(&s.req_mux);

    if (kind == REQ_DTMF) {
        dsp_tone_start_dtmf(t, digit);
    } else if (kind == REQ_RINGTONE) {
        dsp_tone_start_ringtone(t, (uint8_t)digit);
    } else if (kind == REQ_TONE) {
        switch (tone) {
        case AUDIO_TONE_DIAL:     dsp_tone_start(t, DSP_TONE_DIAL); break;
        case AUDIO_TONE_RINGBACK: dsp_tone_start(t, DSP_TONE_RINGBACK); break;
        case AUDIO_TONE_BUSY:     dsp_tone_start(t, DSP_TONE_BUSY); break;
        case AUDIO_TONE_RING:     dsp_tone_start(t, DSP_TONE_RING); break;
        case AUDIO_TONE_KEYCLICK: dsp_tone_start(t, DSP_TONE_KEYCLICK); break;
        case AUDIO_TONE_CALL_WAITING: dsp_tone_start(t, DSP_TONE_CALL_WAITING); break;
        case AUDIO_TONE_NONE:
        default:                  dsp_tone_stop(t); break;
        }
    }
}

/* Write n 16-bit samples in 10 ms chunks. */
static void tx_write(const int16_t *buf, size_t n)
{
#if CONFIG_SIPWIRE_QEMU
    /* QEMU has no I2S: discard, paced like the DMA would be. */
    audio_aec_tx(buf, n);
    vTaskDelay(pdMS_TO_TICKS(n * 1000 / AUDIO_I2S_RATE));
    return;
#endif
    while (n > 0) {
        size_t chunk = n > I2S_CHUNK_SAMPLES ? I2S_CHUNK_SAMPLES : n;
        size_t written = 0;
        esp_err_t err = i2s_channel_write(s.tx, buf, chunk * sizeof(int16_t), &written, I2S_IO_TIMEOUT_MS);
        if (err == ESP_ERR_TIMEOUT) {
            if (atomic_load(&s.stop)) {
                return;
            }
        } else if (err != ESP_OK) {
            ESP_LOGW(TAG, "i2s write: %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(10));
            return;
        }
        written /= sizeof(int16_t);
        audio_aec_tx(buf, written);     /* AEC reference: exactly what the speaker plays */
        buf += written;
        n -= written;
    }
}

static void tx_task(void *arg)
{
    (void)arg;
    int16_t src[AUDIO_FRAME_SAMPLES];
    int32_t mix[AUDIO_FRAME_SAMPLES];
    int16_t pcm16[I2S_FRAME_SAMPLES];
    dsp_tone_t tone;
    dsp_tone_t alert;               /* audio_play_alert: own slot, on top of the tone */

    dsp_tone_init(&tone, AUDIO_SAMPLE_RATE);
    dsp_tone_init(&alert, AUDIO_SAMPLE_RATE);
    alert.amp = ALERT_AMP;

    /* Prime the DMA with silence, then wake the amp: no turn-on pop. */
    memset(pcm16, 0, sizeof(pcm16));
    tx_write(pcm16, I2S_FRAME_SAMPLES);
    amp_set(true);

    int32_t vol_g = dsp_gain_q15(atomic_load(&s.volume));
    int32_t fade = 0;
    int idle_frames = 0;

    for (;;) {
        bool stopping = atomic_load(&s.stop);
        if (stopping && fade == 0) {
            break;
        }
        int32_t fade_to = stopping ? 0 : DSP_GAIN_UNITY;     /* 20 ms fade in / out */

        bool idle_stopping = atomic_load(&s.idle_stopping);
        if (!idle_stopping) {
            tone_poll(&tone);       /* while stopping, a request stays queued for idle_stop_cb */
        }

        memset(src, 0, sizeof(src));
        xSemaphoreTake(s.cb_mtx, portMAX_DELAY);
        bool have_cb = s.src != NULL || s.sink != NULL;
        if (s.src) {
            s.src(src, s.ctx);
        }
        xSemaphoreGive(s.cb_mtx);

        int alert_req = atomic_exchange(&s.alert_req, 0);
        if (alert_req > 0 && have_cb && !stopping) {
            dsp_tone_start(&alert, DSP_TONE_WEAK_SIGNAL);   /* the only alert so far */
        }

        if (have_cb || dsp_tone_active(&tone) || stopping || idle_stopping) {
            idle_frames = 0;
        } else if (++idle_frames >= IDLE_STOP_FRAMES) {
            idle_frames = 0;
            atomic_store(&s.idle_stopping, true);
            esp_timer_start_once(s.idle_timer, 0);
        }

        for (int i = 0; i < AUDIO_FRAME_SAMPLES; i++) {
            mix[i] = src[i];
        }
        dsp_tone_add(&tone, mix, AUDIO_FRAME_SAMPLES);
        dsp_tone_add(&alert, mix, AUDIO_FRAME_SAMPLES);

        int32_t vol_to = dsp_gain_q15(atomic_load(&s.volume));
        dsp_gain_ramp(mix, AUDIO_FRAME_SAMPLES,
                      (int32_t)(((int64_t)vol_g * fade) >> 15),
                      (int32_t)(((int64_t)vol_to * fade_to) >> 15));
        vol_g = vol_to;
        fade = fade_to;

        dsp_softclip_buf(mix, pcm16, AUDIO_FRAME_SAMPLES);
        tx_write(pcm16, I2S_FRAME_SAMPLES);
    }

    /* Faded out; flush one silent frame, then cut the amp. */
    memset(pcm16, 0, sizeof(pcm16));
    tx_write(pcm16, I2S_FRAME_SAMPLES);
    amp_set(false);

    xEventGroupSetBits(s.done, EVT_TX_DONE);
    vTaskDelete(NULL);
}

static void rx_task(void *arg)
{
    (void)arg;
    int32_t raw[I2S_FRAME_SAMPLES];
    int16_t pcm16[I2S_FRAME_SAMPLES];
    dsp_dcblock_t dc;
    int shift = s.cfg->mic.shift;
    int warmup = RX_WARMUP_FRAMES;

    if (shift < 0) {
        shift = 0;
    } else if (shift > 31) {
        shift = 31;
    }
    dsp_dcblock_init(&dc, AUDIO_I2S_RATE, DC_BLOCK_HZ);

    while (!atomic_load(&s.stop)) {
        size_t have = 0;
        while (have < I2S_FRAME_SAMPLES && !atomic_load(&s.stop)) {
            size_t want = I2S_FRAME_SAMPLES - have;
            if (want > I2S_CHUNK_SAMPLES) {
                want = I2S_CHUNK_SAMPLES;
            }
            size_t got = 0;
#if CONFIG_SIPWIRE_QEMU
            /* QEMU has no I2S: silence, paced like the DMA would be. */
            vTaskDelay(pdMS_TO_TICKS(want * 1000 / AUDIO_I2S_RATE));
            memset(&raw[have], 0, want * sizeof(int32_t));
            got = want * sizeof(int32_t);
            esp_err_t err = ESP_OK;
#else
            esp_err_t err = i2s_channel_read(s.rx, &raw[have], want * sizeof(int32_t), &got, I2S_IO_TIMEOUT_MS);
#endif
            if (err == ESP_ERR_TIMEOUT) {
                continue;
            }
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "i2s read: %s", esp_err_to_name(err));
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            have += got / sizeof(int32_t);
        }
        if (have < I2S_FRAME_SAMPLES) {
            break;
        }

        for (int i = 0; i < I2S_FRAME_SAMPLES; i++) {
            raw[i] >>= shift;
        }
        dsp_dcblock_process(&dc, raw, pcm16, I2S_FRAME_SAMPLES);

        if (warmup > 0) {
            warmup--;
            memset(pcm16, 0, sizeof(pcm16));
        }
        /* echo canceller (no-op when off or nobody takes the mic); s.sink is
         * only a hint here, a stale read costs one frame of (non-)processing */
        audio_aec_rx(pcm16, s.sink != NULL);
        if (atomic_load(&s.mic_mute)) {
            memset(pcm16, 0, sizeof(pcm16));
        }

        xSemaphoreTake(s.cb_mtx, portMAX_DELAY);
        if (s.sink) {
            s.sink(pcm16, s.ctx);
        }
        xSemaphoreGive(s.cb_mtx);
    }

    xEventGroupSetBits(s.done, EVT_RX_DONE);
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ */
/* start / stop                                                       */
/* ------------------------------------------------------------------ */

static void post_tone_req(tone_req_kind_t kind, audio_tone_t tone, char digit);

/* Caller holds api_mtx. */
static esp_err_t stream_start_locked(void)
{
    if (atomic_load(&s.running)) {
        return ESP_OK;
    }
    esp_err_t ret = ESP_OK;
    EventBits_t wait_bits = EVT_TX_DONE;
    bool tx_en = false, rx_en = false, tx_task_up = false;

    atomic_store(&s.stop, false);
    atomic_store(&s.idle_stopping, false);
    xEventGroupClearBits(s.done, EVT_TX_DONE | EVT_RX_DONE);

    audio_aec_session_start();          /* I2S sample indices restart with the channels */

    /* a tone request left over from a previous session must not fire now */
    portENTER_CRITICAL(&s.req_mux);
    s.req_kind = REQ_NONE;
    portEXIT_CRITICAL(&s.req_mux);
    atomic_store(&s.alert_req, 0);

    ESP_GOTO_ON_ERROR(i2s_channel_enable(s.tx), fail, TAG, "enable tx");
    tx_en = true;
    if (s.rx) {
        ESP_GOTO_ON_ERROR(i2s_channel_enable(s.rx), fail, TAG, "enable rx");
        rx_en = true;
    }

    if (xTaskCreatePinnedToCore(tx_task, "audio_tx", TX_STACK_BYTES, NULL, AUDIO_TASK_PRIO,
                                NULL, AUDIO_TASK_CORE) != pdPASS) {
        ret = ESP_ERR_NO_MEM;
        ESP_LOGE(TAG, "tx task create failed");
        goto fail;
    }
    tx_task_up = true;
    if (s.rx) {
        if (xTaskCreatePinnedToCore(rx_task, "audio_rx", RX_STACK_BYTES, NULL, AUDIO_TASK_PRIO,
                                    NULL, AUDIO_TASK_CORE) != pdPASS) {
            ret = ESP_ERR_NO_MEM;
            ESP_LOGE(TAG, "rx task create failed");
            goto fail;
        }
    }

    atomic_store(&s.running, true);
    ESP_LOGI(TAG, "streaming started (%s)", s.cfg->mic.full_duplex ? "full duplex" : "2 controllers");
    return ESP_OK;

fail:
    if (tx_task_up) {
        atomic_store(&s.stop, true);
        xEventGroupWaitBits(s.done, wait_bits, pdTRUE, pdTRUE, portMAX_DELAY);
    }
    if (rx_en) {
        i2s_channel_disable(s.rx);
    }
    if (tx_en) {
        i2s_channel_disable(s.tx);
    }
    amp_set(false);
    return ret;
}

/* Caller holds api_mtx. */
static void stream_stop_locked(void)
{
    if (!atomic_load(&s.running)) {
        return;
    }
    EventBits_t bits = EVT_TX_DONE | (s.rx ? EVT_RX_DONE : 0);

    atomic_store(&s.stop, true);        /* TX fades out over 20 ms, then both exit */
    xEventGroupWaitBits(s.done, bits, pdTRUE, pdTRUE, portMAX_DELAY);
    audio_aec_session_end();

    if (s.rx) {
        i2s_channel_disable(s.rx);
    }
    i2s_channel_disable(s.tx);
    amp_set(false);
    atomic_store(&s.running, false);
    ESP_LOGI(TAG, "streaming stopped");
}

/* esp_timer task, armed by tx_task after IDLE_STOP_FRAMES of a tone-only stream with no tone
 * playing. Blocks the timer task for the 20 ms fade-out. Under api_mtx, so it cannot race
 * audio_start/audio_stop: it only stops if still no callback is installed. */
static void idle_stop_cb(void *arg)
{
    (void)arg;
    xSemaphoreTake(s.api_mtx, portMAX_DELAY);
    if (atomic_load(&s.running) && atomic_load(&s.idle_stopping)) {
        xSemaphoreTake(s.cb_mtx, portMAX_DELAY);
        bool have_cb = s.src != NULL || s.sink != NULL;
        xSemaphoreGive(s.cb_mtx);
        if (!have_cb) {
            ESP_LOGI(TAG, "idle: stopping tone-only stream");
            stream_stop_locked();
            /* a tone requested on the fast path meanwhile is still queued: play it */
            portENTER_CRITICAL(&s.req_mux);
            tone_req_kind_t kind = s.req_kind;
            audio_tone_t tone = s.req_tone;
            char digit = s.req_digit;
            portEXIT_CRITICAL(&s.req_mux);
            if (kind != REQ_NONE && !(kind == REQ_TONE && tone == AUDIO_TONE_NONE) &&
                stream_start_locked() == ESP_OK) {
                post_tone_req(kind, tone, digit);
            }
        }
    }
    atomic_store(&s.idle_stopping, false);
    xSemaphoreGive(s.api_mtx);
}

/* ------------------------------------------------------------------ */
/* public API                                                         */
/* ------------------------------------------------------------------ */

esp_err_t audio_init(void)
{
    if (s.inited) {
        return ESP_OK;
    }
    esp_err_t ret = ESP_OK;

    s.cfg = board_cfg();
    ESP_RETURN_ON_FALSE(s.cfg, ESP_ERR_INVALID_STATE, TAG, "no board config");

    s.api_mtx = xSemaphoreCreateMutex();
    s.cb_mtx = xSemaphoreCreateMutex();
    s.done = xEventGroupCreate();
    ESP_GOTO_ON_FALSE(s.api_mtx && s.cb_mtx && s.done, ESP_ERR_NO_MEM, err, TAG, "no memory");

    /* speaker sample i is heard at mic sample i + one full TX DMA ring (see audio_aec.h) */
    ESP_GOTO_ON_ERROR(audio_aec_init(I2S_DMA_DESC_NUM * I2S_CHUNK_SAMPLES, I2S_CHUNK_SAMPLES), err, TAG,
                      "aec init");
    const esp_timer_create_args_t idle_args = { .callback = idle_stop_cb, .name = "audio_idle" };
    ESP_GOTO_ON_ERROR(esp_timer_create(&idle_args, &s.idle_timer), err, TAG, "idle timer");
    ESP_GOTO_ON_ERROR(amp_init(), err, TAG, "amp init");
    ESP_GOTO_ON_ERROR(i2s_setup(), err, TAG, "i2s setup");

    s.inited = true;
    ESP_LOGI(TAG, "ready: i2s %d Hz, spk bclk=%d ws=%d dout=%d, mic din=%d (%s slot), shift %d",
             AUDIO_I2S_RATE, s.cfg->spk.bclk, s.cfg->spk.ws, s.cfg->spk.dout, s.cfg->mic.din,
             s.cfg->mic.slot_right ? "right" : "left", s.cfg->mic.shift);
    return ESP_OK;
err:
    if (s.idle_timer) {
        esp_timer_delete(s.idle_timer);
        s.idle_timer = NULL;
    }
    if (s.api_mtx) {
        vSemaphoreDelete(s.api_mtx);
        s.api_mtx = NULL;
    }
    if (s.cb_mtx) {
        vSemaphoreDelete(s.cb_mtx);
        s.cb_mtx = NULL;
    }
    if (s.done) {
        vEventGroupDelete(s.done);
        s.done = NULL;
    }
    return ret;
}

esp_err_t audio_start(audio_source_cb_t src, audio_sink_cb_t sink, void *ctx)
{
    ESP_RETURN_ON_FALSE(s.inited, ESP_ERR_INVALID_STATE, TAG, "not initialised");

    xSemaphoreTake(s.api_mtx, portMAX_DELAY);

    /* swap callbacks; waits for a running callback to finish */
    xSemaphoreTake(s.cb_mtx, portMAX_DELAY);
    s.src = src;
    s.sink = sink;
    s.ctx = ctx;
    xSemaphoreGive(s.cb_mtx);

    esp_err_t ret = stream_start_locked();
    if (ret != ESP_OK) {
        xSemaphoreTake(s.cb_mtx, portMAX_DELAY);
        s.src = NULL;
        s.sink = NULL;
        s.ctx = NULL;
        xSemaphoreGive(s.cb_mtx);
    }
    xSemaphoreGive(s.api_mtx);
    return ret;
}

esp_err_t audio_stop(void)
{
    ESP_RETURN_ON_FALSE(s.inited, ESP_ERR_INVALID_STATE, TAG, "not initialised");

    xSemaphoreTake(s.api_mtx, portMAX_DELAY);
    stream_stop_locked();
    xSemaphoreTake(s.cb_mtx, portMAX_DELAY);
    s.src = NULL;
    s.sink = NULL;
    s.ctx = NULL;
    xSemaphoreGive(s.cb_mtx);
    xSemaphoreGive(s.api_mtx);
    return ESP_OK;
}

static void post_tone_req(tone_req_kind_t kind, audio_tone_t tone, char digit)
{
    portENTER_CRITICAL(&s.req_mux);
    s.req_kind = kind;
    s.req_tone = tone;
    s.req_digit = digit;
    portEXIT_CRITICAL(&s.req_mux);
}

/* Fast path while streaming (callable from a callback: never takes api_mtx);
 * otherwise auto-start so the tone is audible. */
static void tone_request(tone_req_kind_t kind, audio_tone_t tone, char digit)
{
    if (!s.inited) {
        ESP_LOGW(TAG, "tone requested before audio_init");
        return;
    }
    if (atomic_load(&s.running)) {
        post_tone_req(kind, tone, digit);
        return;
    }
    if (kind == REQ_TONE && tone == AUDIO_TONE_NONE) {
        return;                         /* nothing playing */
    }
    xSemaphoreTake(s.api_mtx, portMAX_DELAY);
    if (stream_start_locked() == ESP_OK) {
        post_tone_req(kind, tone, digit);   /* after start: start clears stale requests */
    }
    xSemaphoreGive(s.api_mtx);
}

void audio_play_tone(audio_tone_t tone)
{
    tone_request(REQ_TONE, tone, 0);
}

void audio_play_alert(audio_alert_t alert)
{
    if (!s.inited || !atomic_load(&s.running)) {
        return;                         /* no stream: no call to warn about */
    }
    atomic_store(&s.alert_req, (int)alert + 1);
}

int audio_ringtone_count(void)
{
    return dsp_ringtone_count();
}

const char *audio_ringtone_name(int idx)
{
    return dsp_ringtone_name(idx);
}

void audio_play_ringtone(int idx)
{
    if (idx < 0 || idx >= dsp_ringtone_count()) {
        idx = 0;
    }
    tone_request(REQ_RINGTONE, AUDIO_TONE_NONE, (char)idx);
}

void audio_stop_ringtone(void)
{
    audio_play_tone(AUDIO_TONE_NONE);
}

void audio_play_dtmf(char digit)
{
    if (!dsp_dtmf_freqs(digit, NULL, NULL)) {
        return;
    }
    tone_request(REQ_DTMF, AUDIO_TONE_NONE, digit);
}

void audio_set_volume(uint8_t volume)
{
    atomic_store(&s.volume, volume > 100 ? 100 : volume);
}

void audio_set_mic_mute(bool mute)
{
    atomic_store(&s.mic_mute, mute);
}
