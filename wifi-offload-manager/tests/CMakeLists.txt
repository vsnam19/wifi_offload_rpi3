find_package(GTest REQUIRED)

# ── Config Loader tests ───────────────────────────────────────────
add_executable(test_config_loader
    test_config_loader.cpp
    ${CMAKE_SOURCE_DIR}/src/config/config_loader.cpp
)
target_include_directories(test_config_loader PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(test_config_loader PRIVATE
    GTest::gtest_main
    nlohmann_json::nlohmann_json
)
target_compile_options(test_config_loader PRIVATE
    -Wall -Wextra -Wpedantic -Wno-unused-parameter
)
add_test(NAME test_config_loader COMMAND test_config_loader)

# ── Path State FSM tests (Phase 4) ───────────────────────────────
add_executable(test_path_state_fsm
    test_path_state_fsm.cpp
    ${CMAKE_SOURCE_DIR}/src/fsm/path_state_fsm.cpp
)
target_include_directories(test_path_state_fsm PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(test_path_state_fsm PRIVATE
    GTest::gtest_main
)
target_compile_options(test_path_state_fsm PRIVATE
    -Wall -Wextra -Wpedantic -Wno-unused-parameter
)
add_test(NAME test_path_state_fsm COMMAND test_path_state_fsm)

# ── Consumer API Server tests (Phase 5) ──────────────────────────
add_executable(test_consumer_api
    test_consumer_api.cpp
    ${CMAKE_SOURCE_DIR}/src/api/consumer_api_server.cpp
)
target_include_directories(test_consumer_api PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(test_consumer_api PRIVATE
    GTest::gtest_main
)
target_compile_options(test_consumer_api PRIVATE
    -Wall -Wextra -Wpedantic -Wno-unused-parameter
)
add_test(NAME test_consumer_api COMMAND test_consumer_api)

# ── Mock consumer (standalone integration helper) ─────────────────
add_executable(mock_consumer
    mock_consumer.cpp
)
target_include_directories(mock_consumer PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)
target_compile_options(mock_consumer PRIVATE
    -Wall -Wextra -Wpedantic -Wno-unused-parameter
)

# ── Routing Policy tests (Phase 2) ───────────────────────────────
# add_executable(test_routing_policy
#     test_routing_policy.cpp
#     ${CMAKE_SOURCE_DIR}/src/routing/routing_policy_manager.cpp
# )
# ...
