idf_component_register(
    SRCS "display.c" "lv_mem_psram.c"
    INCLUDE_DIRS "include"
    # lv_mem_psram.c provides the LVGL allocator: keep it whatever the link order
    WHOLE_ARCHIVE
    PRIV_REQUIRES
        board
        common
        power
        storage
        esp_driver_gpio
        esp_driver_ledc
        esp_driver_spi
        esp_event
        esp_lcd
        esp_timer
        heap
        freertos
        log
)
