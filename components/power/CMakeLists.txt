idf_component_register(
    SRCS "power.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common
    PRIV_REQUIRES cli esp_pm esp_hw_support esp_rom esp_wifi freertos log
)
