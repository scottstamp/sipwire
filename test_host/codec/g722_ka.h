/* Known-answer input shared by test_g722.c and g722tool.c: 200 Hz .. 7.8 kHz
 * linear chirp plus a 440 Hz tone at 16 kHz, peak ~12000. */
#pragma once

#include <math.h>
#include <stddef.h>
#include <stdint.h>

static inline void g722_ka_input(int16_t *x, size_t n)
{
    double ph = 0;
    for (size_t i = 0; i < n; i++) {
        double f = 200.0 + 7600.0 * (double)i / (double)n;
        ph += 2.0 * M_PI * f / 16000.0;
        x[i] = (int16_t)lrint(9000 * sin(ph) + 3000 * sin(2.0 * M_PI * 440.0 * (double)i / 16000.0));
    }
}
