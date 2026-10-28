set(CODEC_DIR ${SIPWIRE_COMPONENTS}/codec)

add_executable(test_codec test_g711.c ${CODEC_DIR}/g711.c)
target_include_directories(test_codec PRIVATE ${CODEC_DIR}/include)
target_link_libraries(test_codec PRIVATE unity)
add_test(NAME codec COMMAND test_codec)

add_executable(test_g722 test_g722.c ${CODEC_DIR}/g722.c ${CODEC_DIR}/g711.c ${CODEC_DIR}/resample2.c)
target_include_directories(test_g722 PRIVATE ${CODEC_DIR}/include)
target_link_libraries(test_g722 PRIVATE unity m)
add_test(NAME g722 COMMAND test_g722)

add_executable(test_resample2 test_resample2.c ${CODEC_DIR}/resample2.c)
target_include_directories(test_resample2 PRIVATE ${CODEC_DIR}/include)
target_link_libraries(test_resample2 PRIVATE unity m)
add_test(NAME resample2 COMMAND test_resample2)

# raw-file encoder/decoder for the Asterisk cross-check (integration.sh), not a ctest
add_executable(g722tool g722tool.c ${CODEC_DIR}/g722.c)
target_include_directories(g722tool PRIVATE ${CODEC_DIR}/include)
target_link_libraries(g722tool PRIVATE m)
