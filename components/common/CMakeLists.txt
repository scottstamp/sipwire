idf_component_register(
    SRCS "sipwire_events.c"
         "sipwire_blf.c"     # pure C, host-tested in test_host/phone
    INCLUDE_DIRS "include"
    REQUIRES esp_event
)
