# tests/CMakeLists.txt — unit test build (stub for Phase 0)
# Phase 1+ will add: test_config_loader, test_path_state_fsm, test_routing_policy

# find_package(GTest REQUIRED)

# add_executable(test_config_loader   test_config_loader.cpp)
# add_executable(test_path_state_fsm  test_path_state_fsm.cpp)
# add_executable(test_routing_policy  test_routing_policy.cpp)

# foreach(t test_config_loader test_path_state_fsm test_routing_policy)
#     target_link_libraries(${t} PRIVATE GTest::gtest_main)
#     target_include_directories(${t} PRIVATE ${CMAKE_SOURCE_DIR}/src)
#     add_test(NAME ${t} COMMAND ${t})
# endforeach()
