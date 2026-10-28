add_executable(test_rtp test_rtp.c
    ${SIPWIRE_COMPONENTS}/rtp/rtp.c
    ${SIPWIRE_COMPONENTS}/codec/g711.c)
target_include_directories(test_rtp PRIVATE
    ${SIPWIRE_COMPONENTS}/rtp/include
    ${SIPWIRE_COMPONENTS}/codec/include)
target_link_libraries(test_rtp PRIVATE unity)
add_test(NAME rtp COMMAND test_rtp)
