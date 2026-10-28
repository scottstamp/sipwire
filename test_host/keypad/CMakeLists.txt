add_executable(test_keypad test_keypad.c ${SIPWIRE_COMPONENTS}/keypad/keypad_scan.c)
target_include_directories(test_keypad PRIVATE ${SIPWIRE_COMPONENTS}/keypad)
target_link_libraries(test_keypad PRIVATE unity)
add_test(NAME keypad COMMAND test_keypad)
