idf_component_register(
    SRCS "ota.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common
    PRIV_REQUIRES esp_https_ota esp_http_client esp_app_format app_update esp_partition esp_system
                  esp_timer esp_event mbedtls cli common net phone storage freertos log
)
