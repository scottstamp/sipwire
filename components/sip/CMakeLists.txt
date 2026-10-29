idf_component_register(
    SRCS "sip_msg.c"
         "sip_md5.c"
         "sip_auth.c"
         "sip_sdp.c"
         "sip_txn.c"
         "sip_dialog.c"
         "sip_ua.c"
         "sip_sub.c"
         "sip_event.c"
         "sip_udp.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "private_include"
    PRIV_REQUIRES lwip    # sip_udp.c only; the protocol core is pure C
)
