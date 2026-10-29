idf_component_register(
    SRCS "audio.c" "audio_aec.c" "audio_dsp.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common
    PRIV_REQUIRES board esp_driver_i2s esp_driver_gpio esp_timer heap freertos log esp_driver_spi aec
)
