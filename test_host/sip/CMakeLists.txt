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

# integration client (integration.sh), not a ctest; speaks RTP for the
# tone check, so it links rtp + codec too
add_executable(sipcli sipcli.c ${SIP_DIR}/sip_udp.c
    ${SIPWIRE_COMPONENTS}/rtp/rtp.c
    ${SIPWIRE_COMPONENTS}/codec/g711.c
    ${SIPWIRE_COMPONENTS}/codec/g722.c
    ${SIPWIRE_COMPONENTS}/codec/resample2.c)
target_include_directories(sipcli PRIVATE
    ${SIPWIRE_COMPONENTS}/rtp/include
    ${SIPWIRE_COMPONENTS}/codec/include)
target_link_libraries(sipcli sip_core m)
