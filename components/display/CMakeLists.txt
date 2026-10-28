idf_component_register(
    SRCS "display.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES
        board
        common
        storage
        esp_driver_gpio
        esp_driver_ledc
        esp_driver_spi
        esp_event
        esp_lcd
        esp_timer
        freertos
        log
)
