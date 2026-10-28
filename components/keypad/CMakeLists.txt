idf_component_register(
    SRCS "keypad.c" "keypad_scan.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES board common esp_driver_gpio esp_timer freertos log
)
