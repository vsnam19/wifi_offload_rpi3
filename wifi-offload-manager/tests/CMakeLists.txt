# ── Mock consumer (standalone integration helper / P6-T1 E2E) ────
add_executable(mock_consumer
    mock_consumer.cpp
)
target_include_directories(mock_consumer PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)
target_compile_options(mock_consumer PRIVATE
    -Wall -Wextra -Wpedantic -Wno-unused-parameter
)
install(TARGETS mock_consumer RUNTIME DESTINATION ${CMAKE_INSTALL_SBINDIR})

# ── FOTA consumer (MPTCP download demo / P7-T1 E2E) ──────────────
# Downloads a large firmware image via MPTCP, registers with the daemon for
# cgroup assignment and PathEvent notifications during the download.
# Link with OpenSSL for optional SHA-256 verification (USE_OPENSSL=ON).
option(USE_OPENSSL "Enable SHA-256 verification in fota_consumer" OFF)

add_executable(fota_consumer
    fota_consumer.cpp
)
target_include_directories(fota_consumer PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)
target_compile_options(fota_consumer PRIVATE
    -Wall -Wextra -Wpedantic -Wno-unused-parameter
)
if(USE_OPENSSL)
    find_package(OpenSSL REQUIRED)
    target_compile_definitions(fota_consumer PRIVATE USE_OPENSSL)
    target_link_libraries(fota_consumer PRIVATE OpenSSL::SSL OpenSSL::Crypto)
endif()
install(TARGETS fota_consumer RUNTIME DESTINATION ${CMAKE_INSTALL_SBINDIR})

if(NOT BUILD_TESTS)
    return()
endif()

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

# ── Routing Policy tests (Phase 2) ───────────────────────────────
add_executable(test_routing_policy
    test_routing_policy.cpp
    ${CMAKE_SOURCE_DIR}/src/routing/routing_policy_manager.cpp
)
target_include_directories(test_routing_policy PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${LIBMNL_INCLUDE_DIR}
    ${LIBIP4TC_INCLUDE_DIR}
)
target_link_libraries(test_routing_policy PRIVATE
    GTest::gtest_main
    ${LIBMNL_LIBRARY}
    ${LIBIP4TC_LIBRARY}
)
target_compile_options(test_routing_policy PRIVATE
    -Wall -Wextra -Wpedantic -Wno-unused-parameter
)
target_compile_definitions(test_routing_policy PRIVATE
    _GNU_SOURCE
)
add_test(NAME test_routing_policy COMMAND test_routing_policy)
