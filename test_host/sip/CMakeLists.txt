# Host tests for components/sip. The protocol core is built as a library and
# linked into several Unity executables; sipcli (integration tool, see
# integration.sh) is built too but is not a ctest.
set(SIP_DIR ${SIPWIRE_COMPONENTS}/sip)

add_library(sip_core STATIC
    ${SIP_DIR}/sip_msg.c
    ${SIP_DIR}/sip_md5.c
    ${SIP_DIR}/sip_auth.c
    ${SIP_DIR}/sip_sdp.c
    ${SIP_DIR}/sip_txn.c
    ${SIP_DIR}/sip_dialog.c
    ${SIP_DIR}/sip_ua.c
)
target_include_directories(sip_core PUBLIC ${SIP_DIR}/include ${SIP_DIR}/private_include)

foreach(t test_sip_msg test_sip_auth test_sip_sdp test_sip_ua test_sip_fuzz)
    add_executable(${t} ${t}.c sip_test_util.c)
    target_link_libraries(${t} sip_core unity)
    add_test(NAME ${t} COMMAND ${t})
endforeach()
