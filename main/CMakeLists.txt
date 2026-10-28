idf_component_register(
    SRCS
        "main.c"
        "display.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        board
        common
        ui
        esp_timer
        log
)
