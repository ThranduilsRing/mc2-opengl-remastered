# tools/object_light_row_harness/CMakeLists.txt
# SUBSYSTEM-HARNESS-ARC / LIGHT-ROW-PURE-1
#
# STANDALONE build (no main build64/ cache, no vcpkg, no deps):
#   cmake -S tools/object_light_row_harness -B build64-h-object_light_row_harness -A x64
#   cmake --build build64-h-object_light_row_harness --config RelWithDebInfo
#
# Links NO production .cpp and NO external deps — #includes the header-only pure
# producer (mclib/object_light_row.h, the same header GatherLightsParameters
# delegates row production to). That is what lets the byte-identity proof run
# with the game unlaunchable.

cmake_minimum_required(VERSION 3.16)
project(object_light_row_harness CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component(MC2_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

add_executable(object_light_row_harness object_light_row_harness.cpp)
target_include_directories(object_light_row_harness PRIVATE
    ${MC2_ROOT}/tools/contract_harness_common
    ${MC2_ROOT}/mclib)                 # for "object_light_row.h"
target_compile_features(object_light_row_harness PRIVATE cxx_std_17)
