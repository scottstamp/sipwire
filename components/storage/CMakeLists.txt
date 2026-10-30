idf_component_register(
    SRCS "settings.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common common
    PRIV_REQUIRES nvs_flash esp_event heap log
)
