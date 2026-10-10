# UNITQUERY-SETTARGETPRIORITY-1: standalone unit-test target for the pure brain
# symbol resolver. Engine-free — compiles only the resolver TU + the test driver.
cmake_minimum_required(VERSION 3.20)
project(brain_symbol_resolve_test CXX)

include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/Mc2Standalone.cmake")

if(NOT DEFINED REPO_ROOT)
    get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
endif()

add_executable(brain_symbol_resolve_test
    main.cpp
    "${REPO_ROOT}/code/brain_symbol_resolve.cpp"
)
mc2_link_gate(brain_symbol_resolve_test)
target_include_directories(brain_symbol_resolve_test PRIVATE "${REPO_ROOT}/code")
