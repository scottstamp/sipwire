# Wi-Fi link policy (components/net/net_policy.c): roam scan trigger and candidate choice, the
# weak-signal warning during calls.
add_executable(test_net test_net_policy.c ${SIPWIRE_COMPONENTS}/net/net_policy.c)
target_include_directories(test_net PRIVATE ${SIPWIRE_COMPONENTS}/net)
target_link_libraries(test_net PRIVATE unity)
add_test(NAME net COMMAND test_net)
