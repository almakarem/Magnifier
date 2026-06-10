include(GoogleTest)

add_executable(magnifier_tests
    test_state_model.cpp
    test_cli_parser.cpp
    test_config_store.cpp
    test_json_framing.cpp
    test_hotkey_parse.cpp
)
target_link_libraries(magnifier_tests PRIVATE
    magnifier_core
    GTest::gtest
    GTest::gtest_main
)
magnifier_target_setup(magnifier_tests)

gtest_discover_tests(magnifier_tests
    DISCOVERY_TIMEOUT 30
    PROPERTIES TIMEOUT 60
)
