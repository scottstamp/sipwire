idf_component_register(
    SRCS
        "main.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        board
        cli
        display
        common
        net
        storage
        ui
        esp_app_format
        esp_event
        esp_timer
        log
        nvs_flash
)
