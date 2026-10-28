#pragma once

#include <stdbool.h>
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_err.h"

/** Board description, filled from CONFIG_SIPWIRE_* (menuconfig → Sipwire board).
 *  Drivers take pins from here; nothing else reads CONFIG_SIPWIRE_* pins. */
typedef struct {
    struct {
        spi_host_device_t host;
        gpio_num_t sclk, mosi, miso, cs, dc, rst, bl;
        bool bl_active_high;
        int pclk_hz;
        int h_res, v_res;
        bool bgr, invert, mirror_x, mirror_y, swap_xy;
    } lcd;
    struct {
        bool present;
        gpio_num_t cs, irq;
        int spi_hz;
    } touch;
    struct {
        gpio_num_t bclk, ws, dout, en;
        bool en_active_high;
    } spk;
    struct {
        bool full_duplex;        /* shares spk bclk/ws on one I2S controller */
        gpio_num_t bclk, ws, din; /* bclk/ws == spk pins when full_duplex */
        bool slot_right;
        int shift;               /* 32-bit sample >> shift = 16-bit */
    } mic;
    struct {
        gpio_num_t rows[4];
        gpio_num_t cols[3];
        char map[12];            /* row-major, e.g. "123456789*0#" */
    } keypad;
    struct {
        bool present;
        bool spi_mode;           /* true: SPI on LCD bus, false: SDMMC 1-bit */
        gpio_num_t clk, cmd, d0; /* SDMMC */
        gpio_num_t cs;           /* SPI */
        gpio_num_t cd;           /* card detect, GPIO_NUM_NC if none */
    } sd;
} board_cfg_t;

const board_cfg_t *board_cfg(void);

/** GPIO ISR service + shared LCD/touch/SD SPI bus. Call once, before any driver. */
esp_err_t board_init(void);
