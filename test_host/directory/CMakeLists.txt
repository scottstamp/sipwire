# Pure logic of components/directory (dir_core.c): number normalisation, contact table,
# CSV, speed dial, history ring + binary format. stub/esp_err.h lets directory.h compile.
add_executable(test_directory test_directory.c ${SIPWIRE_COMPONENTS}/directory/dir_core.c)
target_include_directories(test_directory PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/stub
    ${SIPWIRE_COMPONENTS}/directory/include
    ${SIPWIRE_COMPONENTS}/directory/private_include)
target_link_libraries(test_directory PRIVATE unity)
add_test(NAME directory COMMAND test_directory)
