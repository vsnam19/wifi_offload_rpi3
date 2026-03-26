cmake_minimum_required(VERSION 3.26)
project(wifi-offload-manager VERSION 0.1.0 LANGUAGES CXX)

# ── C++23 ─────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── Dependencies ──────────────────────────────────────────────────
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBMNL REQUIRED libmnl)

# nlohmann/json — header-only; find via cmake config or fall back to pkg-config
find_package(nlohmann_json QUIET)
if(NOT nlohmann_json_FOUND)
    pkg_check_modules(NLOHMANN_JSON REQUIRED nlohmann_json)
endif()

# ── Sources ───────────────────────────────────────────────────────
add_executable(wifi-offload-manager
    src/main.cpp
    src/config/config_loader.cpp
    # Phase 2+:
    # src/routing/routing_policy_manager.cpp
    # src/mptcp/mptcp_manager.cpp
    # src/wpa/wpa_monitor.cpp
    # src/fsm/path_state_fsm.cpp
    # src/api/consumer_api_server.cpp
)

target_include_directories(wifi-offload-manager PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${LIBMNL_INCLUDE_DIRS}
)

target_link_libraries(wifi-offload-manager PRIVATE
    ${LIBMNL_LIBRARIES}
)

if(nlohmann_json_FOUND)
    target_link_libraries(wifi-offload-manager PRIVATE nlohmann_json::nlohmann_json)
else()
    target_include_directories(wifi-offload-manager PRIVATE ${NLOHMANN_JSON_INCLUDE_DIRS})
endif()

# ── Compiler flags ────────────────────────────────────────────────
target_compile_options(wifi-offload-manager PRIVATE
    -Wall
    -Wextra
    -Werror
    -Wpedantic
    -Wno-unused-parameter   # wpa_ctrl callback signatures
)

target_compile_definitions(wifi-offload-manager PRIVATE
    _GNU_SOURCE             # for sigaction, POSIX extensions
)

# ── Install ───────────────────────────────────────────────────────
include(GNUInstallDirs)
install(TARGETS wifi-offload-manager
    RUNTIME DESTINATION ${CMAKE_INSTALL_SBINDIR}
)
install(FILES config/path-policies.json
    DESTINATION ${CMAKE_INSTALL_SYSCONFDIR}/netservice
)

# ── Tests (stubbed — activated when Phase 1 unit tests are written) ──
option(BUILD_TESTS "Build unit tests" ON)
if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
