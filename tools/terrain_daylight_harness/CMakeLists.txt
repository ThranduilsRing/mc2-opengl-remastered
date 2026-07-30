# tools/terrain_daylight_harness/CMakeLists.txt
# TERRAIN-AMBIENT-FLOOR-2
#
# STANDALONE build (no main build64/ cache, no vcpkg, no deps):
#   cmake -S tools/terrain_daylight_harness -B build64-h-terrain_daylight_harness -A x64
#   cmake --build build64-h-terrain_daylight_harness --config RelWithDebInfo
#
# Links NO production .cpp and NO external deps — #includes the header-only pure
# math (GameOS/gameos/utils/terrain_daylight.h), which is the SAME header the
# engine's gamecam.cpp calls. There is no second copy of the gate to drift.

cmake_minimum_required(VERSION 3.16)
project(terrain_daylight_harness CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component(MC2_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

add_executable(terrain_daylight_harness terrain_daylight_harness.cpp)
target_include_directories(terrain_daylight_harness PRIVATE
    ${MC2_ROOT}/tools/contract_harness_common
    ${MC2_ROOT}/GameOS/gameos/utils)        # for "terrain_daylight.h"
target_compile_features(terrain_daylight_harness PRIVATE cxx_std_17)
