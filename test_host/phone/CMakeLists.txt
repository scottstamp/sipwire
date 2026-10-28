# Pure call-control policy (components/phone/phone_logic.c) and the UI-bridge
# model builder (main/ui_bridge_model.c). stubs/ provides esp_event.h and
# esp_err.h so phone.h / sipwire_events.h compile on the host.
set(SIPWIRE_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
add_executable(test_phone test_phone.c
    ${SIPWIRE_COMPONENTS}/phone/phone_logic.c
    ${SIPWIRE_ROOT}/main/ui_bridge_model.c)
target_include_directories(test_phone PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs
    ${SIPWIRE_COMPONENTS}/phone/include
    ${SIPWIRE_COMPONENTS}/phone/private_include
    ${SIPWIRE_COMPONENTS}/sip/include
    ${SIPWIRE_COMPONENTS}/rtp/include
    ${SIPWIRE_COMPONENTS}/common/include
    ${SIPWIRE_COMPONENTS}/ui/include
    ${SIPWIRE_ROOT}/main)
target_link_libraries(test_phone PRIVATE unity)
add_test(NAME phone COMMAND test_phone)
