idf_component_register(
    SRCS "net.c"
         "net_policy.c"     # pure C, host-tested in test_host/net
    INCLUDE_DIRS "include"
    REQUIRES esp_common
    PRIV_REQUIRES esp_eth esp_wifi esp_netif esp_event esp_timer common storage cli log
)
