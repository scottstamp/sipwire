#pragma once

#include <stdbool.h>
#include "esp_err.h"

/* Power management policy (docs/ARCHITECTURE.md "Power").
 *
 * Dynamic frequency scaling (CONFIG_PM_ENABLE + CONFIG_SIPWIRE_POWER_DFS): the CPU runs at
 * CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ while any hold below is active and drops to
 * CONFIG_SIPWIRE_POWER_MIN_CPU_MHZ (80: APB stays at 80 MHz, so UART, LEDC, I2S, SPI and
 * RMT clocks never change) when none is. Automatic light sleep is never enabled.
 * Wi-Fi modem sleep belongs to net (net_set_low_latency); the phone switches both together. */

typedef enum {
    POWER_HOLD_CALL = 0,    /* any call exists (ringing, active, held, two calls, ended screen) */
    POWER_HOLD_DISPLAY,     /* backlight on or dimmed: LVGL renders at full speed */
    POWER_HOLD_COUNT,
} power_hold_t;

/** Configure DFS (unless the QEMU build or CONFIG_SIPWIRE_POWER_DFS=n) and register the
 *  `power` console command. Call early, before anything calls power_hold(). */
esp_err_t power_init(void);

/** Keep the CPU at full speed for `who` (true) or let it go (false). Idempotent per reason,
 *  callable from any task (not from an ISR). Before power_init() or without DFS only the
 *  state is tracked (shown by `power`). */
void power_hold(power_hold_t who, bool hold);

/** Bitmask of active holds (1 << power_hold_t). */
unsigned power_holds(void);
