# dir_core.c is pure C (host-tested in test_host/directory); the rest needs IDF.
idf_component_register(
    SRCS "dir_core.c" "directory.c" "dir_console.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "private_include"
    REQUIRES esp_common
    PRIV_REQUIRES cli console fatfs wear_levelling heap freertos log
)
