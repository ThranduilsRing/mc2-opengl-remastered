# tools/shader_contract_harness/CMakeLists.txt
# SUBSYSTEM-HARNESS-ARC / SHADER-CONTRACT-HARNESS-1
#
# STANDALONE build (does NOT touch the main build64/ cache, no vcpkg, no deps):
#   cmake -S tools/shader_contract_harness -B build64-shader -G "Visual Studio 17 2022" -A x64
#   cmake --build build64-shader --config RelWithDebInfo --target shader_contract_harness
#   build64-shader/RelWithDebInfo/shader_contract_harness.exe --list
#
# Links NO production code and pulls NO external dependencies (C++17 stdlib only,
# incl. <filesystem>). The linker is the proof it stays game-free. The repo root
# is baked in as MC2_REPO_ROOT so the harness can find shaders/ and engine
# source regardless of cwd; env MC2_CONTRACT_REPO_ROOT overrides at runtime.

cmake_minimum_required(VERSION 3.16)
project(shader_contract_harness CXX)

include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/Mc2Standalone.cmake")

get_filename_component(MC2_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

add_executable(shader_contract_harness shader_contract_harness.cpp)
target_include_directories(shader_contract_harness PRIVATE
    ${MC2_ROOT}/tools/contract_harness_common)
target_compile_definitions(shader_contract_harness PRIVATE
    MC2_REPO_ROOT="${MC2_ROOT}")
target_compile_features(shader_contract_harness PRIVATE cxx_std_20)
