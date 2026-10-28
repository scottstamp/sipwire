add_executable(test_status_led test_status_led.c ${SIPWIRE_COMPONENTS}/status_led/status_led_pattern.c)
target_include_directories(test_status_led PRIVATE ${SIPWIRE_COMPONENTS}/status_led)
target_link_libraries(test_status_led unity)
add_test(NAME status_led COMMAND test_status_led)
