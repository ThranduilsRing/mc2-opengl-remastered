# tools/gate_dump/CMakeLists.txt
#
# STANDALONE build (no main cache, no vcpkg, no deps):
#   cmake -S tools/gate_dump -B build64-gate-dump -A x64
#   cmake --build build64-gate-dump --config RelWithDebInfo
#   build64-gate-dump/RelWithDebInfo/gate_dump.exe > Gates/gate_table.json
#
# Compiles the gate table (Gates/gate_table.cpp) and prints it as JSON, so every consumer of
# the table reads what the COMPILER saw. GATE_INDEX_BOOTSTRAP skips the index cross-check:
# a row added to the table can be dumped before the generated index is refreshed.

cmake_minimum_required(VERSION 3.16)
project(gate_dump CXX)

include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/Mc2Standalone.cmake")

get_filename_component(MC2_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

add_executable(gate_dump
    main.cpp
    ${MC2_ROOT}/Gates/gate.cpp
    ${MC2_ROOT}/Gates/gate_table.cpp)
target_include_directories(gate_dump PRIVATE ${MC2_ROOT}/Gates)
target_compile_definitions(gate_dump PRIVATE GATE_INDEX_BOOTSTRAP _CRT_SECURE_NO_WARNINGS)
if(MSVC)
    target_compile_options(gate_dump PRIVATE /utf-8)
endif()
