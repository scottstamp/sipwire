add_executable(test_ui_lock test_ui_lock.c
    ${SIPWIRE_COMPONENTS}/ui/ui_lock_logic.c)
target_include_directories(test_ui_lock PRIVATE
    ${SIPWIRE_COMPONENTS}/ui
    ${SIPWIRE_COMPONENTS}/ui/include)
target_link_libraries(test_ui_lock PRIVATE unity)
add_test(NAME ui_lock COMMAND test_ui_lock)
