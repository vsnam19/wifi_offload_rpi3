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

# ── Path State FSM tests (Phase 4 — P4-T5) ───────────────────────
add_executable(test_path_state_fsm
    test_path_state_fsm.cpp
    ${CMAKE_SOURCE_DIR}/src/fsm/path_state_fsm.cpp
)
target_include_directories(test_path_state_fsm PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/wpa   # for wpa_ctrl.h transitively included by wpa_monitor.hpp
)
target_compile_definitions(test_path_state_fsm PRIVATE
    CONFIG_CTRL_IFACE        # required by wpa_ctrl.h header guard
    CONFIG_CTRL_IFACE_UNIX
)
target_link_libraries(test_path_state_fsm PRIVATE
    GTest::gtest_main
)
target_compile_options(test_path_state_fsm PRIVATE
    -Wall -Wextra -Wpedantic -Wno-unused-parameter
)
add_test(NAME test_path_state_fsm COMMAND test_path_state_fsm)

# ── Routing Policy tests (Phase 2) ───────────────────────────────
# add_executable(test_routing_policy
#     test_routing_policy.cpp
#     ${CMAKE_SOURCE_DIR}/src/routing/routing_policy_manager.cpp
# )
# ...
