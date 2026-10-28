add_executable(test_ui_touch_cal test_ui_touch_cal.c
    ${SIPWIRE_COMPONENTS}/ui/ui_touch_cal.c)
target_include_directories(test_ui_touch_cal PRIVATE
    ${SIPWIRE_COMPONENTS}/ui
    ${SIPWIRE_COMPONENTS}/ui/include)
target_link_libraries(test_ui_touch_cal PRIVATE unity)
add_test(NAME ui_touch_cal COMMAND test_ui_touch_cal)
