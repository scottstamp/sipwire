#pragma once

#include <stdbool.h>
#include "esp_err.h"

/* Power management policy (docs/ARCHITECTURE.md "Power").
 *
 * Dynamic frequency scaling (CONFIG_PM_ENABLE + CONFIG_SIPWIRE_POWER_DFS): the CPU runs at
 * CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ while a `call` or `display` hold is active and drops to
 * CONFIG_SIPWIRE_POWER_MIN_CPU_MHZ (80: APB stays at 80 MHz, so UART, LEDC, I2S, SPI and
 * RMT clocks never change) when neither is.
 *
 * Automatic light sleep (CONFIG_SIPWIRE_POWER_LIGHT_SLEEP, needs FreeRTOS tickless idle): with
 * no hold at all the chip light-sleeps whenever both cores idle; Wi-Fi stays associated (modem
 * sleep, wakes for DTIM beacons), the keypad columns and the touch PENIRQ are GPIO wake
 * sources, the console UART wakes it on RX edges and then keeps it awake (`console` hold).
 * Every hold blocks light sleep. Wi-Fi modem sleep belongs to net (net_set_low_latency); the
 * phone switches both together. */

typedef enum {
    POWER_HOLD_CALL = 0,    /* any call exists (ringing, active, held, two calls, ended screen) */
    POWER_HOLD_DISPLAY,     /* backlight on or dimmed: LVGL renders at full speed */
    POWER_HOLD_CONSOLE,     /* console input recently (power.c only): no light sleep, DFS stays */
    POWER_HOLD_COUNT,
} power_hold_t;

/** Configure DFS and light sleep (unless the QEMU build or the Kconfig options are off),
 *  the light-sleep wake sources and the `power` console command. Call early, before
 *  anything calls power_hold(). */
esp_err_t power_init(void);

/** Keep the CPU at full speed and the chip out of light sleep for `who` (true) or let it go
 *  (false). Idempotent per reason, callable from any task (not from an ISR). Before
 *  power_init() or without DFS only the state is tracked (shown by `power`). */
void power_hold(power_hold_t who, bool hold);

/** Bitmask of active holds (1 << power_hold_t). */
unsigned power_holds(void);
