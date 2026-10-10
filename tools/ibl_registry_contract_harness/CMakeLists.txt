# tools/ibl_registry_contract_harness/CMakeLists.txt
# SUBSYSTEM-HARNESS-ARC / IBL-REGISTRY-CONTRACT-HARNESS-1
#
# STANDALONE build (does NOT touch the main build64/ cache, no vcpkg, no deps):
#   cmake -S tools/ibl_registry_contract_harness -B build64-ibl -G "Visual Studio 17 2022" -A x64
#   cmake --build build64-ibl --config RelWithDebInfo --target ibl_registry_contract_harness
#   build64-ibl/RelWithDebInfo/ibl_registry_contract_harness.exe --list
#
# Cheapest harness in the arc: links NO production .cpp and NO external deps —
# it #includes the header-only constexpr registries directly (RenderCore/
# IblHdriRegistry.h + IblShRegistry.h, which are "no GL, no game-side headers").
# The linker is the proof it stays game-free. Repo root is baked as MC2_REPO_ROOT
# for the asset-existence checks; env MC2_CONTRACT_REPO_ROOT overrides.

cmake_minimum_required(VERSION 3.16)
project(ibl_registry_contract_harness CXX)

include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/Mc2Standalone.cmake")

get_filename_component(MC2_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

add_executable(ibl_registry_contract_harness ibl_registry_contract_harness.cpp)
target_include_directories(ibl_registry_contract_harness PRIVATE
    ${MC2_ROOT}/tools/contract_harness_common
    ${MC2_ROOT})                       # for "RenderCore/IblHdriRegistry.h" style
target_compile_definitions(ibl_registry_contract_harness PRIVATE
    MC2_REPO_ROOT="${MC2_ROOT}")
target_compile_features(ibl_registry_contract_harness PRIVATE cxx_std_20)
mc2_link_gate(ibl_registry_contract_harness)

