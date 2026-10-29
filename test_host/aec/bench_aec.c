/* Host micro-benchmark of the echo canceller: µs per 20 ms frame for a few
 * tail lengths, with and without the residual suppressor. Informational only
 * (not a ctest): build_host/aec/bench_aec */

#include <stdio.h>
#include <stdint.h>
#include <time.h>

#include "aec.h"

#define FRAMES  1500                    /* 30 s */

static uint32_t s_seed = 1;

static int16_t rnd16(int amp)
{
    s_seed = s_seed * 1664525u + 1013904223u;
    return (int16_t)(((int32_t)(s_seed >> 16) - 32768) * amp / 32768);
}

static double now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e6 + (double)ts.tv_nsec / 1e3;
}

int main(void)
{
    static int16_t far[AEC_FRAME_SAMPLES], mic[AEC_FRAME_SAMPLES], out[AEC_FRAME_SAMPLES];
    for (int sup = 0; sup <= 2; sup++) {
        /* sup 2: suppressor on, silent mic and reference (what QEMU's smoke test feeds it) */
        for (uint32_t tail = sup == 2 ? 1280 : 640; tail <= (sup == 2 ? 1280u : 3200u); tail += 320) {
            aec_config_t c;
            aec_config_default(&c);
            c.tail_samples = tail;
            if (!sup) {
                c.echo_suppress_db = c.echo_suppress_active_db = c.noise_suppress_db = 0;
            }
            aec_t *a = NULL;
            if (aec_create(&c, &a) != AEC_OK) {
                return 1;
            }
            double worst = 0, t_all = now_us();
            for (int f = 0; f < FRAMES; f++) {
                for (int i = 0; i < AEC_FRAME_SAMPLES; i++) {
                    far[i] = sup == 2 ? 0 : rnd16(8000);
                    mic[i] = sup == 2 ? 0 : (int16_t)(far[i] / 3 + rnd16(500));
                }
                double t0 = now_us();
                aec_process(a, mic, far, out);
                double dt = now_us() - t0;
                if (dt > worst) {
                    worst = dt;
                }
            }
            t_all = now_us() - t_all;
            const char *mode = sup == 2 ? "silent" : sup ? "+suppress" : "linear";
            printf("tail %4u (%2u partitions) %-10s: avg %6.1f us/frame, max %6.1f us, %zu bytes\n", (unsigned)tail,
                   (unsigned)(tail / AEC_FRAME_SAMPLES), mode, t_all / FRAMES, worst, aec_mem_bytes(a));
            aec_destroy(a);
        }
    }
    return 0;
}
