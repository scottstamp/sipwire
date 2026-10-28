add_executable(test_codec test_g711.c ${SIPWIRE_COMPONENTS}/codec/g711.c)
target_include_directories(test_codec PRIVATE ${SIPWIRE_COMPONENTS}/codec/include)
target_link_libraries(test_codec PRIVATE unity)
add_test(NAME codec COMMAND test_codec)
