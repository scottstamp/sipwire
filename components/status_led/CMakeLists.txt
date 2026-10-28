idf_component_register(
    SRCS "status_led.c" "status_led_pattern.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common
    PRIV_REQUIRES board common esp_driver_rmt esp_event esp_timer log
)
