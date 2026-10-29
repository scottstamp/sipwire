#include "audio_aec.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <string.h>

#include "audio.h"
#include "audio_dsp.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "audio";

#if CONFIG_SIPWIRE_AEC

#include "esp_aec.h"

#if CONFIG_SIPWIRE_AEC_MODE_FD_HIGH_PERF
#define AEC_MODE                AEC_MODE_FD_HIGH_PERF
#elif CONFIG_SIPWIRE_AEC_MODE_VOIP_LOW_COST
#define AEC_MODE                AEC_MODE_VOIP_LOW_COST
#elif CONFIG_SIPWIRE_AEC_MODE_VOIP_HIGH_PERF
#define AEC_MODE                AEC_MODE_VOIP_HIGH_PERF
#else
#define AEC_MODE                AEC_MODE_FD_LOW_COST
#endif

#define AEC_NLP_DEFAULT         AEC_NLP_LEVEL_AGGR      /* esp-sr's default */
#define RING_SAMPLES            4096                    /* 256 ms of speaker history */
#define DELAY_MAX               (RING_SAMPLES - 3 * AUDIO_FRAME_SAMPLES)
/* A block counts for ERL/ERLE when the reference is above ~-50 dBFS RMS. */
#define REF_ACTIVE_MEANSQ       (100 * 100)
#define RECENT_SAMPLES          (2 * AUDIO_SAMPLE_RATE) /* ERLE window: 2 s of far-end signal */
/* esp-sr allocates part of its state from PSRAM whatever aec_config_t.caps
 * says, and does not check for NULL: make sure it cannot run out. */
#define PSRAM_NEEDED            (256 * 1024)
#define INTERNAL_NEEDED         (48 * 1024)

typedef struct {
    uint32_t frames, blocks, ref_late;
    uint64_t busy_us;
    uint32_t max_us;
    uint64_t e_mic, e_out, e_ref;               /* blocks with far-end signal, whole run */
    uint64_t w_mic, w_out;                      /* current ERLE window */
    uint32_t w_samples;
    float erle_recent_db;
} run_stats_t;

static struct {
    SemaphoreHandle_t mtx;                      /* serialises create / settings calls */
    uint32_t default_delay, xrun_shift;

    /* set once by create(), then published through `ready` */
    aec_handle_t *h;
    uint32_t block, latency;
    dsp_ring_t ring;
    dsp_blockfifo_t fifo;
    int16_t *ref_frame;
    void *bufs[6];
    uint32_t mem_internal, mem_psram;
    atomic_bool ready;

    atomic_bool enabled;
    _Atomic int32_t delay_override;             /* < 0: default */
    _Atomic int32_t nlp_level, nlp_req;         /* nlp_req < 0: nothing pending */

    /* per stream */
    _Atomic uint32_t tx_pos;                    /* TX task */
    _Atomic uint32_t tx_xruns, rx_xruns;        /* ISR */
    uint32_t rx_pos;                            /* RX task only */
    bool running;                               /* RX task only */
    run_stats_t run;                            /* RX task only */
    uint32_t delay_now;                         /* RX task only */

    portMUX_TYPE mux;                           /* guards the published copy below */
    run_stats_t pub;
    bool pub_running;
    uint32_t pub_delay;
} a = {
    .mux = portMUX_INITIALIZER_UNLOCKED,
    .delay_override = -1,
    .nlp_level = AEC_NLP_DEFAULT,
    .nlp_req = -1,
};

/* ------------------------------------------------------------------ */
/* setup                                                              */
/* ------------------------------------------------------------------ */

static void *alloc_buf(size_t samples, uint32_t caps)
{
    for (size_t i = 0; i < sizeof(a.bufs) / sizeof(a.bufs[0]); i++) {
        if (a.bufs[i] == NULL) {
            a.bufs[i] = heap_caps_aligned_calloc(16, samples, sizeof(int16_t), caps);
            return a.bufs[i];
        }
    }
    return NULL;
}

static void free_all(void)
{
    if (a.h) {
        aec_destroy(a.h);
        a.h = NULL;
    }
    for (size_t i = 0; i < sizeof(a.bufs) / sizeof(a.bufs[0]); i++) {
        heap_caps_free(a.bufs[i]);
        a.bufs[i] = NULL;
    }
}

/* Caller holds a.mtx. */
static esp_err_t create(void)
{
    const uint32_t big_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    const uint32_t hot_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;   /* touched every block */
    const size_t int0 = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const size_t ps0 = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    esp_err_t ret = ESP_OK;

    ESP_RETURN_ON_FALSE(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) >= PSRAM_NEEDED &&
                        int0 >= INTERNAL_NEEDED, ESP_ERR_NO_MEM, TAG,
                        "aec needs %d KB PSRAM + %d KB internal free", PSRAM_NEEDED / 1024, INTERNAL_NEEDED / 1024);

    aec_config_t cfg = {
        .mic_num = 1,
        .ref_num = 1,
        .out_num = 1,
        .filter_length = CONFIG_SIPWIRE_AEC_FILTER_LEN,
        .sample_rate = AUDIO_SAMPLE_RATE,
        .caps = big_caps,
        .mode = AEC_MODE,
        .nlp_level = (aec_nlp_level_t)atomic_load(&a.nlp_level),
    };
    a.h = aec_create_from_config(&cfg);
    ESP_GOTO_ON_FALSE(a.h, ESP_ERR_NO_MEM, fail, TAG, "aec create failed");
    int block = aec_get_chunksize(a.h);
    ESP_GOTO_ON_FALSE(block > 0 && block <= 2048, ESP_ERR_INVALID_SIZE, fail, TAG, "aec chunk %d", block);
    a.block = (uint32_t)block;

    int16_t *ring = alloc_buf(RING_SAMPLES, big_caps);
    int16_t *mic = alloc_buf(a.block, hot_caps);
    int16_t *ref = alloc_buf(a.block, hot_caps);
    int16_t *proc = alloc_buf(a.block, hot_caps);
    int16_t *out = alloc_buf(dsp_blockfifo_out_size(AUDIO_FRAME_SAMPLES, a.block), hot_caps);
    a.ref_frame = alloc_buf(AUDIO_FRAME_SAMPLES, hot_caps);
    ESP_GOTO_ON_FALSE(ring && mic && ref && proc && out && a.ref_frame, ESP_ERR_NO_MEM, fail, TAG, "aec buffers");
    dsp_ring_init(&a.ring, ring, RING_SAMPLES);
    dsp_blockfifo_init(&a.fifo, AUDIO_FRAME_SAMPLES, a.block, mic, ref, proc, out);
    a.latency = (uint32_t)a.fifo.latency;

    a.mem_internal = (uint32_t)(int0 - heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    a.mem_psram = (uint32_t)(ps0 - heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    atomic_store_explicit(&a.ready, true, memory_order_release);
    ESP_LOGI(TAG, "aec: %s, filter %d, %" PRIu32 "-sample blocks (+%" PRIu32 " samples latency), "
             "heap internal %" PRIu32 " B, psram %" PRIu32 " B",
             aec_get_mode_string(AEC_MODE), CONFIG_SIPWIRE_AEC_FILTER_LEN, a.block, a.latency,
             a.mem_internal, a.mem_psram);
    return ESP_OK;
fail:
    free_all();
    return ret;
}

esp_err_t audio_aec_init(uint32_t default_delay, uint32_t xrun_shift)
{
    if (a.mtx == NULL) {
        a.mtx = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(a.mtx, ESP_ERR_NO_MEM, TAG, "no memory");
    }
    a.default_delay = default_delay;
    a.xrun_shift = xrun_shift;
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* audio task hooks                                                   */
/* ------------------------------------------------------------------ */

void audio_aec_session_start(void)
{
    atomic_store(&a.tx_pos, 0);
    atomic_store(&a.tx_xruns, 0);
    atomic_store(&a.rx_xruns, 0);
    a.rx_pos = 0;
    a.running = false;
    if (atomic_load_explicit(&a.ready, memory_order_acquire)) {
        dsp_ring_init(&a.ring, a.ring.buf, RING_SAMPLES);   /* indices restart: forget old audio */
    }
    portENTER_CRITICAL(&a.mux);
    a.pub_running = false;
    portEXIT_CRITICAL(&a.mux);
}

void audio_aec_session_end(void)
{
    a.running = false;
    portENTER_CRITICAL(&a.mux);
    a.pub_running = false;
    portEXIT_CRITICAL(&a.mux);
}

void audio_aec_xrun(bool tx)
{
    atomic_fetch_add(tx ? &a.tx_xruns : &a.rx_xruns, 1);
}

void audio_aec_tx(const int16_t *pcm, size_t n)
{
    uint32_t pos = atomic_load_explicit(&a.tx_pos, memory_order_relaxed);
    if (atomic_load_explicit(&a.ready, memory_order_acquire)) {
        dsp_ring_write(&a.ring, pos, pcm, n);
    }
    atomic_store_explicit(&a.tx_pos, pos + (uint32_t)n, memory_order_release);
}

static void block_fn(void *ctx, int16_t *mic, int16_t *ref, int16_t *out)
{
    run_stats_t *r = ctx;
    int64_t t0 = esp_timer_get_time();
#if CONFIG_SIPWIRE_QEMU
    /* Espressif's QEMU itself segfaults executing esp-sr's processing code:
     * everything around the canceller runs, the canceller passes through. */
    memcpy(out, mic, a.block * sizeof(int16_t));
#else
    aec_process(a.h, mic, ref, out);
#endif
    uint32_t dt = (uint32_t)(esp_timer_get_time() - t0);

    r->blocks++;
    r->busy_us += dt;
    if (dt > r->max_us) {
        r->max_us = dt;
    }

    uint64_t e_ref = dsp_energy(ref, a.block);
    if (e_ref < (uint64_t)REF_ACTIVE_MEANSQ * a.block) {
        return;                         /* no far-end signal: nothing to measure */
    }
    uint64_t e_mic = dsp_energy(mic, a.block);
    uint64_t e_out = dsp_energy(out, a.block);
    r->e_ref += e_ref;
    r->e_mic += e_mic;
    r->e_out += e_out;
    r->w_mic += e_mic;
    r->w_out += e_out;
    r->w_samples += a.block;
    if (r->w_samples >= RECENT_SAMPLES) {
        r->erle_recent_db = dsp_ratio_db(r->w_mic, r->w_out);
        r->w_mic = r->w_out = 0;
        r->w_samples = 0;
    }
}

/* Reference delay for this frame: the configured offset, moved by one DMA
 * buffer per I2S queue overflow (a TX overflow plays a buffer late, an RX
 * overflow drops captured samples). */
static uint32_t current_delay(void)
{
    int32_t d = atomic_load(&a.delay_override);
    if (d < 0) {
        d = (int32_t)a.default_delay;
    }
    d += (int32_t)a.xrun_shift * ((int32_t)atomic_load(&a.tx_xruns) - (int32_t)atomic_load(&a.rx_xruns));
    if (d < 0) {
        d = 0;
    } else if (d > DELAY_MAX) {
        d = DELAY_MAX;
    }
    return (uint32_t)d;
}

void audio_aec_rx(int16_t *pcm, bool want)
{
    uint32_t pos = a.rx_pos;
    a.rx_pos += AUDIO_FRAME_SAMPLES;

    bool run = want && atomic_load(&a.enabled) && atomic_load_explicit(&a.ready, memory_order_acquire);
    if (!run) {
        if (a.running) {
            a.running = false;
            portENTER_CRITICAL(&a.mux);
            a.pub_running = false;
            portEXIT_CRITICAL(&a.mux);
        }
        return;
    }
    if (!a.running) {
        /* (re)start: fresh latency padding and stats; the filter keeps what it learned */
        dsp_blockfifo_reset(&a.fifo);
        memset(&a.run, 0, sizeof(a.run));
        a.running = true;
    }
    int32_t nlp = atomic_exchange(&a.nlp_req, -1);
    if (nlp >= 0) {
        aec_set_nlp_level(a.h, (aec_nlp_level_t)nlp);
    }

    a.delay_now = current_delay();
    uint32_t written = atomic_load_explicit(&a.tx_pos, memory_order_acquire);
    a.run.ref_late += dsp_ring_read(&a.ring, pos - a.delay_now, written, a.ref_frame, AUDIO_FRAME_SAMPLES);
    dsp_blockfifo_process(&a.fifo, pcm, a.ref_frame, pcm, block_fn, &a.run);
    a.run.frames++;

    portENTER_CRITICAL(&a.mux);
    a.pub = a.run;
    a.pub_running = true;
    a.pub_delay = a.delay_now;
    portEXIT_CRITICAL(&a.mux);
}

/* ------------------------------------------------------------------ */
/* public API                                                         */
/* ------------------------------------------------------------------ */

esp_err_t audio_set_aec(bool on)
{
    ESP_RETURN_ON_FALSE(a.mtx, ESP_ERR_INVALID_STATE, TAG, "audio not initialised");
    esp_err_t ret = ESP_OK;
    xSemaphoreTake(a.mtx, portMAX_DELAY);
    if (on && !atomic_load(&a.ready)) {
        ret = create();
    }
    if (ret == ESP_OK && on != atomic_load(&a.enabled)) {
        atomic_store(&a.enabled, on);
        ESP_LOGI(TAG, "aec %s", on ? "on" : "off");
    }
    xSemaphoreGive(a.mtx);
    return ret;
}

esp_err_t audio_set_aec_delay(int samples)
{
    ESP_RETURN_ON_FALSE(samples >= -1 && samples <= DELAY_MAX, ESP_ERR_INVALID_ARG, TAG,
                        "aec delay: -1 or 0..%d samples", DELAY_MAX);
    atomic_store(&a.delay_override, (int32_t)samples);
    return ESP_OK;
}

esp_err_t audio_set_aec_nlp(int level)
{
    ESP_RETURN_ON_FALSE(level >= AEC_NLP_LEVEL_NORMAL && level <= AEC_NLP_LEVEL_VERYAGGR, ESP_ERR_INVALID_ARG,
                        TAG, "aec nlp: 0..2");
    atomic_store(&a.nlp_level, (int32_t)level);
    atomic_store(&a.nlp_req, (int32_t)level);      /* applied by the RX task (or at create) */
    return ESP_OK;
}

void audio_get_aec_stats(audio_aec_stats_t *out)
{
    run_stats_t r;
    memset(out, 0, sizeof(*out));
    portENTER_CRITICAL(&a.mux);
    r = a.pub;
    out->running = a.pub_running;
    out->delay_samples = a.pub_delay;
    portEXIT_CRITICAL(&a.mux);

    out->built = true;
    out->enabled = atomic_load(&a.enabled);
    out->ready = atomic_load_explicit(&a.ready, memory_order_acquire);
    out->mode = aec_get_mode_string(AEC_MODE);
#if CONFIG_SIPWIRE_QEMU
    out->bypassed = true;
#endif
    out->nlp_level = (int)atomic_load(&a.nlp_level);
    out->delay_override = atomic_load(&a.delay_override) >= 0;
    if (!out->running) {
        int32_t d = atomic_load(&a.delay_override);
        out->delay_samples = d < 0 ? a.default_delay : (uint32_t)d;
    }
    if (out->ready) {
        out->block_samples = a.block;
        out->latency_samples = a.latency;
        out->mem_internal = a.mem_internal;
        out->mem_psram = a.mem_psram;
    }
    out->frames = r.frames;
    out->blocks = r.blocks;
    out->avg_us = r.frames ? (uint32_t)(r.busy_us / r.frames) : 0;
    out->max_us = r.max_us;
    out->ref_late = r.ref_late;
    out->tx_xruns = atomic_load(&a.tx_xruns);
    out->rx_xruns = atomic_load(&a.rx_xruns);
    out->erl_db = dsp_ratio_db(r.e_ref, r.e_mic);
    out->erle_db = dsp_ratio_db(r.e_mic, r.e_out);
    out->erle_recent_db = r.erle_recent_db;
}

#else /* !CONFIG_SIPWIRE_AEC: hooks are no-ops, the mic path is untouched */

esp_err_t audio_aec_init(uint32_t default_delay, uint32_t xrun_shift)
{
    (void)default_delay;
    (void)xrun_shift;
    return ESP_OK;
}

void audio_aec_session_start(void) {}
void audio_aec_session_end(void) {}
void audio_aec_tx(const int16_t *pcm, size_t n) { (void)pcm; (void)n; }
void audio_aec_rx(int16_t *pcm, bool want) { (void)pcm; (void)want; }
void audio_aec_xrun(bool tx) { (void)tx; }

esp_err_t audio_set_aec(bool on)
{
    return on ? ESP_ERR_NOT_SUPPORTED : ESP_OK;
}

esp_err_t audio_set_aec_delay(int samples)
{
    (void)samples;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t audio_set_aec_nlp(int level)
{
    (void)level;
    return ESP_ERR_NOT_SUPPORTED;
}

void audio_get_aec_stats(audio_aec_stats_t *out)
{
    memset(out, 0, sizeof(*out));
}

#endif
