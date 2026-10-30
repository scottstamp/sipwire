add_executable(test_ui_fmt test_ui_fmt.c
    ${SIPWIRE_COMPONENTS}/ui/ui_fmt.c)
target_include_directories(test_ui_fmt PRIVATE ${SIPWIRE_COMPONENTS}/ui ${SIPWIRE_COMPONENTS}/ui/include)
target_link_libraries(test_ui_fmt PRIVATE unity)
add_test(NAME ui_fmt COMMAND test_ui_fmt)
