add_executable(test_rtp test_rtp.c
    ${SIPWIRE_COMPONENTS}/rtp/rtp.c
    ${SIPWIRE_COMPONENTS}/codec/g711.c
    ${SIPWIRE_COMPONENTS}/codec/g722.c
    ${SIPWIRE_COMPONENTS}/codec/resample2.c)
target_include_directories(test_rtp PRIVATE
    ${SIPWIRE_COMPONENTS}/rtp/include
    ${SIPWIRE_COMPONENTS}/codec/include)
target_link_libraries(test_rtp PRIVATE unity m)
add_test(NAME rtp COMMAND test_rtp)
# Tens of simulated minutes of 16 kHz audio through the resamplers/codecs:
# ~30 s unoptimised under ASan, ~20 s at -O1.
target_compile_options(test_rtp PRIVATE -O1)
