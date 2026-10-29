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
# Minutes of simulated 16 kHz audio through the resamplers/codecs, plus the
# fuzz loops: ~1.5 s at -O2 under ASan (the drift and long-run cases were cut
# from 10 to 3 simulated minutes to keep host tests short).
target_compile_options(test_rtp PRIVATE -O2)
