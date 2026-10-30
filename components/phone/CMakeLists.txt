idf_component_register(
    SRCS "phone.c"
         "phone_media.c"
         "phone_console.c"
         "phone_logic.c"    # pure C, host-tested in test_host/phone
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "private_include"
    REQUIRES common rtp esp_common
    PRIV_REQUIRES directory sip audio storage net power cli esp_event esp_timer esp_hw_support freertos heap lwip log vfs
)
