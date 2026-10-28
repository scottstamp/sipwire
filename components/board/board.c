#include "board.h"

#include <string.h>
#include "esp_check.h"
#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "board";

#define PIN(x) ((gpio_num_t)(x))

/* IS_ENABLED(CONFIG_FOO): 1 if the bool option is set, else 0 (Linux kernel trick). */
#define _ARG_PLACEHOLDER_1 0,
#define _TAKE_SECOND(ignored, val, ...) val
#define _IS_DEFINED3(arg1_or_junk) _TAKE_SECOND(arg1_or_junk 1, 0)
#define _IS_DEFINED2(val) _IS_DEFINED3(_ARG_PLACEHOLDER_##val)
#define IS_ENABLED(option) _IS_DEFINED2(option)

#ifdef CONFIG_SIPWIRE_I2S_FULL_DUPLEX
#define MIC_BCLK CONFIG_SIPWIRE_SPK_BCLK
#define MIC_WS   CONFIG_SIPWIRE_SPK_WS
#else
#define MIC_BCLK CONFIG_SIPWIRE_MIC_BCLK
#define MIC_WS   CONFIG_SIPWIRE_MIC_WS
#endif

static board_cfg_t s_cfg = {
    .lcd = {
        .host = (spi_host_device_t)(CONFIG_SIPWIRE_LCD_SPI_HOST - 1),
        .sclk = PIN(CONFIG_SIPWIRE_LCD_SCLK),
        .mosi = PIN(CONFIG_SIPWIRE_LCD_MOSI),
        .miso = PIN(CONFIG_SIPWIRE_LCD_MISO),
        .cs = PIN(CONFIG_SIPWIRE_LCD_CS),
        .dc = PIN(CONFIG_SIPWIRE_LCD_DC),
        .rst = PIN(CONFIG_SIPWIRE_LCD_RST),
        .bl = PIN(CONFIG_SIPWIRE_LCD_BL),
        .bl_active_high = IS_ENABLED(CONFIG_SIPWIRE_LCD_BL_ACTIVE_HIGH),
        .pclk_hz = CONFIG_SIPWIRE_LCD_PCLK_HZ,
        .h_res = CONFIG_SIPWIRE_LCD_H_RES,
        .v_res = CONFIG_SIPWIRE_LCD_V_RES,
        .bgr = IS_ENABLED(CONFIG_SIPWIRE_LCD_BGR),
        .invert = IS_ENABLED(CONFIG_SIPWIRE_LCD_INVERT),
        .mirror_x = IS_ENABLED(CONFIG_SIPWIRE_LCD_MIRROR_X),
        .mirror_y = IS_ENABLED(CONFIG_SIPWIRE_LCD_MIRROR_Y),
        .swap_xy = IS_ENABLED(CONFIG_SIPWIRE_LCD_SWAP_XY),
    },
    .touch = {
#ifdef CONFIG_SIPWIRE_TOUCH_ENABLE
        .present = true,
        .cs = PIN(CONFIG_SIPWIRE_TOUCH_CS),
        .irq = PIN(CONFIG_SIPWIRE_TOUCH_IRQ),
        .spi_hz = CONFIG_SIPWIRE_TOUCH_SPI_HZ,
#else
        .cs = GPIO_NUM_NC,
        .irq = GPIO_NUM_NC,
#endif
    },
    .spk = {
        .bclk = PIN(CONFIG_SIPWIRE_SPK_BCLK),
        .ws = PIN(CONFIG_SIPWIRE_SPK_WS),
        .dout = PIN(CONFIG_SIPWIRE_SPK_DOUT),
        .en = PIN(CONFIG_SIPWIRE_SPK_EN),
        .en_active_high = IS_ENABLED(CONFIG_SIPWIRE_SPK_EN_ACTIVE_HIGH),
    },
    .mic = {
        .full_duplex = IS_ENABLED(CONFIG_SIPWIRE_I2S_FULL_DUPLEX),
        .bclk = PIN(MIC_BCLK),
        .ws = PIN(MIC_WS),
        .din = PIN(CONFIG_SIPWIRE_MIC_DIN),
        .slot_right = IS_ENABLED(CONFIG_SIPWIRE_MIC_SLOT_RIGHT),
        .shift = CONFIG_SIPWIRE_MIC_SHIFT,
    },
    .keypad = {
        .rows = { PIN(CONFIG_SIPWIRE_KP_ROW0), PIN(CONFIG_SIPWIRE_KP_ROW1),
                  PIN(CONFIG_SIPWIRE_KP_ROW2), PIN(CONFIG_SIPWIRE_KP_ROW3) },
        .cols = { PIN(CONFIG_SIPWIRE_KP_COL0), PIN(CONFIG_SIPWIRE_KP_COL1),
                  PIN(CONFIG_SIPWIRE_KP_COL2) },
    },
    .sd = {
#ifdef CONFIG_SIPWIRE_SD_ENABLE
        .present = true,
        .spi_mode = IS_ENABLED(CONFIG_SIPWIRE_SD_MODE_SPI),
#ifdef CONFIG_SIPWIRE_SD_MODE_SDMMC
        .clk = PIN(CONFIG_SIPWIRE_SD_CLK),
        .cmd = PIN(CONFIG_SIPWIRE_SD_CMD),
        .d0 = PIN(CONFIG_SIPWIRE_SD_D0),
        .cs = GPIO_NUM_NC,
#else
        .clk = GPIO_NUM_NC,
        .cmd = GPIO_NUM_NC,
        .d0 = GPIO_NUM_NC,
        .cs = PIN(CONFIG_SIPWIRE_SD_CS),
#endif
        .cd = PIN(CONFIG_SIPWIRE_SD_CD),
#else
        .clk = GPIO_NUM_NC, .cmd = GPIO_NUM_NC, .d0 = GPIO_NUM_NC,
        .cs = GPIO_NUM_NC, .cd = GPIO_NUM_NC,
#endif
    },
};

const board_cfg_t *board_cfg(void)
{
    return &s_cfg;
}

esp_err_t board_init(void)
{
    const char *map = CONFIG_SIPWIRE_KP_MAP;
    ESP_RETURN_ON_FALSE(strlen(map) == 12, ESP_ERR_INVALID_ARG, TAG, "KP_MAP must be 12 chars");
    memcpy(s_cfg.keypad.map, map, 12);

    esp_err_t err = gpio_install_isr_service(0);
    ESP_RETURN_ON_FALSE(err == ESP_OK || err == ESP_ERR_INVALID_STATE, err, TAG, "isr service");

    const spi_bus_config_t bus = {
        .sclk_io_num = s_cfg.lcd.sclk,
        .mosi_io_num = s_cfg.lcd.mosi,
        .miso_io_num = s_cfg.lcd.miso,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = s_cfg.lcd.h_res * 40 * 2,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(s_cfg.lcd.host, &bus, SPI_DMA_CH_AUTO), TAG, "spi bus");

    ESP_LOGI(TAG, "lcd spi%d sclk=%d mosi=%d miso=%d, touch=%d, sd=%s",
             s_cfg.lcd.host + 1, s_cfg.lcd.sclk, s_cfg.lcd.mosi, s_cfg.lcd.miso,
             s_cfg.touch.present, !s_cfg.sd.present ? "none" : s_cfg.sd.spi_mode ? "spi" : "sdmmc");
    return ESP_OK;
}
