# Directory is "cli", not "console": a project component named "console" would
# shadow ESP-IDF's own `console` component (esp_console) that this one builds on.
idf_component_register(
    SRCS "console.c"
    INCLUDE_DIRS "include"
    REQUIRES console
    PRIV_REQUIRES esp_system esp_timer heap freertos storage net log
)
