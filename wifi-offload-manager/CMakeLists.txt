cmake_minimum_required(VERSION 3.26)
project(wifi-offload-manager VERSION 0.1.0 LANGUAGES C CXX)

# ── C++23 ─────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── Dependencies ──────────────────────────────────────────────────
# Use find_library/find_path for libmnl — works in both native and Yocto
# cross-compile environments without requiring pkg-config in the sysroot.
find_library(LIBMNL_LIBRARY NAMES mnl REQUIRED)
find_path(LIBMNL_INCLUDE_DIR NAMES libmnl/libmnl.h REQUIRED)

# libip4tc (iptables legacy C API) — for P2-T2 iptables mangle rule management.
# Provides iptc_init/iptc_append_entry/iptc_commit etc. without shelling out.
find_library(LIBIP4TC_LIBRARY NAMES ip4tc REQUIRED)
find_path(LIBIP4TC_INCLUDE_DIR NAMES libiptc/libiptc.h REQUIRED)

# nlohmann/json — header-only; CMake config package (preferred in Yocto sysroot)
find_package(nlohmann_json QUIET)
if(NOT nlohmann_json_FOUND)
    # Fallback: locate the header directory directly
    find_path(NLOHMANN_JSON_INCLUDE_DIR NAMES nlohmann/json.hpp REQUIRED)
endif()

# ── Sources ───────────────────────────────────────────────────────
add_executable(wifi-offload-manager
    src/main.cpp
    src/config/config_loader.cpp
    src/routing/routing_policy_manager.cpp  # P2-T1
    src/mptcp/mptcp_manager.cpp              # Phase 4 (brought forward)
    src/wpa/wpa_ctrl.c                       # P3-T1: upstream wpa_ctrl (unmodified)
    src/wpa/wpa_monitor.cpp                  # P3-T2..T5
    src/fsm/path_state_fsm.cpp               # P4-T1/T2
    src/api/consumer_api_server.cpp           # P5-T1..T6
)

target_include_directories(wifi-offload-manager PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/wpa   # for wpa_ctrl.c: includes.h, common.h, os.h
    ${LIBMNL_INCLUDE_DIR}
    ${LIBIP4TC_INCLUDE_DIR}
)

target_link_libraries(wifi-offload-manager PRIVATE
    ${LIBMNL_LIBRARY}
    ${LIBIP4TC_LIBRARY}
)

if(nlohmann_json_FOUND)
    target_link_libraries(wifi-offload-manager PRIVATE nlohmann_json::nlohmann_json)
else()
    target_include_directories(wifi-offload-manager PRIVATE ${NLOHMANN_JSON_INCLUDE_DIR})
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
    CONFIG_CTRL_IFACE       # enable wpa_ctrl compile
    CONFIG_CTRL_IFACE_UNIX  # use Unix domain socket
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
