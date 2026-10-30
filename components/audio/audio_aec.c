#include "audio_aec.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "audio.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "audio";

#if CONFIG_SIPWIRE_AEC

#include "aec.h"

_Static_assert(AUDIO_FRAME_SAMPLES == AEC_FRAME_SAMPLES && AUDIO_SAMPLE_RATE == AEC_SAMPLE_RATE,
               "the canceller works on the audio path's frames");

#define RING_SAMPLES            4096                    /* 256 ms of speaker history */
#define DELAY_MAX               (RING_SAMPLES - 3 * AUDIO_FRAME_SAMPLES)
/* The canceller's state is touched every frame: internal RAM, but leave this
 * much internal heap to Wi-Fi / lwIP / drivers; the rest goes to PSRAM. */
#define INTERNAL_RESERVE        (64 * 1024)

typedef struct {
    uint32_t frames, ref_late;
    uint64_t busy_us;
    uint32_t max_us;
    aec_stats_t aec;
} run_stats_t;

static struct {
    SemaphoreHandle_t mtx;                      /* serialises create / settings calls */
    uint32_t default_delay, xrun_shift;

    /* set once by create(), then published through `ready` */
    aec_t *aec;
    aec_ring_t ring;
    uint32_t tail;
    uint32_t mem_internal, mem_psram;
    atomic_bool ready;

    atomic_bool enabled;
    _Atomic int32_t delay_override;             /* < 0: default */
    portMUX_TYPE supp_mux;                      /* guards supp / supp_pending */
    int supp[3];                                /* echo, echo active, noise (dB, <= 0) */
    bool supp_pending;                          /* RX task applies it */

    /* per stream */
    _Atomic uint32_t tx_pos;                    /* TX task */
    _Atomic uint32_t tx_xruns, rx_xruns;        /* ISR */
    uint32_t rx_pos;                            /* RX task only */
    bool running;                               /* RX task only */
    run_stats_t run;                            /* RX task only */
    int16_t ref_frame[AUDIO_FRAME_SAMPLES];     /* RX task only */

    portMUX_TYPE mux;                           /* guards the published copy below */
    run_stats_t pub;
    bool pub_running;
    uint32_t pub_delay, pub_latency;
} a = {
    .mux = portMUX_INITIALIZER_UNLOCKED,
    .supp_mux = portMUX_INITIALIZER_UNLOCKED,
    .delay_override = -1,
    .supp = { AEC_ECHO_SUPPRESS_DEFAULT, AEC_ECHO_SUPPRESS_ACTIVE_DEFAULT, 0 },
};

/* ------------------------------------------------------------------ */
/* setup                                                              */
/* ------------------------------------------------------------------ */

/* Allocator for the canceller (aec_set_allocator) and the ring; only called
 * from create(), under a.mtx. Zeroed memory, as SpeexDSP expects. */
static void *aec_heap_alloc(size_t size)
{
    if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL) >= size + INTERNAL_RESERVE) {
        void *p = heap_caps_calloc(1, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (p) {
            a.mem_internal += (uint32_t)size;
            return p;
        }
    }
    void *p = heap_caps_calloc(1, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p) {
        a.mem_psram += (uint32_t)size;
    }
    return p;
}

/* Caller holds a.mtx. */
static esp_err_t create(void)
{
    aec_config_t cfg;
    aec_config_default(&cfg);
    cfg.tail_samples = CONFIG_SIPWIRE_AEC_TAIL;
    portENTER_CRITICAL(&a.supp_mux);
    cfg.echo_suppress_db = a.supp[0];
    cfg.echo_suppress_active_db = a.supp[1];
    cfg.noise_suppress_db = a.supp[2];
    a.supp_pending = false;
    portEXIT_CRITICAL(&a.supp_mux);

    /* SpeexDSP does not check its allocations: make sure they cannot fail
     * (the estimate is an upper bound, plus heap overhead for ~65 blocks). */
    const size_t need = aec_mem_estimate(cfg.tail_samples) + RING_SAMPLES * sizeof(int16_t) + 2048;
    const size_t internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const size_t avail = (internal > INTERNAL_RESERVE ? internal - INTERNAL_RESERVE : 0) +
                         heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    ESP_RETURN_ON_FALSE(avail >= need, ESP_ERR_NO_MEM, TAG, "aec needs %u B of heap, %u available",
                        (unsigned)need, (unsigned)avail);

    /* aec_heap_alloc is greedy (internal until INTERNAL_RESERVE is left): the canceller's
     * state, touched all over every 20 ms frame, goes first; the ring (one sequential
     * 640 B write and read per frame) takes what is left, i.e. PSRAM unless all fits. */
    a.mem_internal = a.mem_psram = 0;
    aec_set_allocator(aec_heap_alloc, heap_caps_free);
    aec_err_t err = aec_create(&cfg, &a.aec);
    if (err != AEC_OK) {
        ESP_LOGE(TAG, "aec create: %d", (int)err);
        return err == AEC_ERR_NO_MEM ? ESP_ERR_NO_MEM : ESP_ERR_INVALID_ARG;
    }
    int16_t *ring = aec_heap_alloc(RING_SAMPLES * sizeof(int16_t));
    if (ring == NULL) {
        aec_destroy(a.aec);
        a.aec = NULL;
        a.mem_internal = a.mem_psram = 0;
        ESP_LOGE(TAG, "aec ring: no memory");
        return ESP_ERR_NO_MEM;
    }
    aec_ring_init(&a.ring, ring, RING_SAMPLES);
    aec_get_config(a.aec, &cfg);
    a.tail = cfg.tail_samples;

    atomic_store_explicit(&a.ready, true, memory_order_release);
    ESP_LOGI(TAG, "aec: SpeexDSP, tail %" PRIu32 ", suppress %d/%d/%d dB, heap internal %" PRIu32
             " B, psram %" PRIu32 " B", a.tail, cfg.echo_suppress_db, cfg.echo_suppress_active_db,
             cfg.noise_suppress_db, a.mem_internal, a.mem_psram);
    return ESP_OK;
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
        aec_ring_init(&a.ring, a.ring.buf, RING_SAMPLES);   /* indices restart: forget old audio */
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
        aec_ring_write(&a.ring, pos, pcm, n);
    }
    atomic_store_explicit(&a.tx_pos, pos + (uint32_t)n, memory_order_release);
}

/* Nominal speaker->mic delay for this frame: the configured offset, moved by
 * one DMA buffer per I2S queue overflow (a TX overflow plays a buffer late,
 * an RX overflow drops captured samples). */
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
        /* (re)start: fresh stats; the filter keeps what it learned */
        memset(&a.run, 0, sizeof(a.run));
        aec_clear_stats(a.aec);
        a.running = true;
    }
    if (a.supp_pending) {               /* racy peek; the copy below is under the lock */
        int s[3];
        portENTER_CRITICAL(&a.supp_mux);
        memcpy(s, a.supp, sizeof(s));
        a.supp_pending = false;
        portEXIT_CRITICAL(&a.supp_mux);
        aec_set_suppress(a.aec, s[0], s[1], s[2]);
    }

    uint32_t delay = current_delay();
    uint32_t written = atomic_load_explicit(&a.tx_pos, memory_order_acquire);
    a.run.ref_late += aec_ring_read(&a.ring, pos - aec_ref_offset(delay), written, a.ref_frame,
                                    AUDIO_FRAME_SAMPLES);

    int64_t t0 = esp_timer_get_time();
    aec_process(a.aec, pcm, a.ref_frame, pcm);
    uint32_t dt = (uint32_t)(esp_timer_get_time() - t0);

    a.run.frames++;
    a.run.busy_us += dt;
    if (dt > a.run.max_us) {
        a.run.max_us = dt;
    }
    aec_get_stats(a.aec, &a.run.aec);

    portENTER_CRITICAL(&a.mux);
    a.pub = a.run;
    a.pub_running = true;
    a.pub_delay = delay;
    a.pub_latency = aec_latency(a.aec);
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

esp_err_t audio_set_aec_suppress(int echo_db, int echo_active_db, int noise_db)
{
    const int s[3] = { -abs(echo_db), -abs(echo_active_db), -abs(noise_db) };
    for (int i = 0; i < 3; i++) {
        ESP_RETURN_ON_FALSE(s[i] >= AEC_SUPPRESS_MIN_DB, ESP_ERR_INVALID_ARG, TAG, "aec suppress: 0..%d dB",
                            -AEC_SUPPRESS_MIN_DB);
    }
    portENTER_CRITICAL(&a.supp_mux);
    bool changed = memcmp(s, a.supp, sizeof(s)) != 0;
    memcpy(a.supp, s, sizeof(s));
    a.supp_pending = a.supp_pending || changed;     /* applied by the RX task (or at create) */
    portEXIT_CRITICAL(&a.supp_mux);
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
    out->latency_samples = a.pub_latency;
    portEXIT_CRITICAL(&a.mux);

    out->built = true;
    out->enabled = atomic_load(&a.enabled);
    out->ready = atomic_load_explicit(&a.ready, memory_order_acquire);
    out->tail_samples = out->ready ? a.tail : (uint32_t)CONFIG_SIPWIRE_AEC_TAIL;
    portENTER_CRITICAL(&a.supp_mux);
    out->suppress_db = a.supp[0];
    out->suppress_active_db = a.supp[1];
    out->noise_db = a.supp[2];
    portEXIT_CRITICAL(&a.supp_mux);
    out->delay_override = atomic_load(&a.delay_override) >= 0;
    if (!out->running) {
        int32_t d = atomic_load(&a.delay_override);
        out->delay_samples = d < 0 ? a.default_delay : (uint32_t)d;
        out->latency_samples = (out->suppress_db || out->suppress_active_db || out->noise_db) ?
                               AEC_FRAME_SAMPLES : 0;
    }
    if (out->ready) {
        out->mem_internal = a.mem_internal;
        out->mem_psram = a.mem_psram;
    }
    out->frames = r.frames;
    out->avg_us = r.frames ? (uint32_t)(r.busy_us / r.frames) : 0;
    out->max_us = r.max_us;
    out->ref_late = r.ref_late;
    out->resets = r.aec.resets;
    out->tx_xruns = atomic_load(&a.tx_xruns);
    out->rx_xruns = atomic_load(&a.rx_xruns);
    out->erl_db = aec_ratio_db(r.aec.e_ref, r.aec.e_mic);
    out->erle_db = aec_ratio_db(r.aec.e_mic, r.aec.e_out);
    out->erle_recent_db = r.aec.erle_recent_db;
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

esp_err_t audio_set_aec_suppress(int echo_db, int echo_active_db, int noise_db)
{
    (void)echo_db;
    (void)echo_active_db;
    (void)noise_db;
    return ESP_ERR_NOT_SUPPORTED;
}

void audio_get_aec_stats(audio_aec_stats_t *out)
{
    memset(out, 0, sizeof(*out));
}

#endif
