# tools/class_light_band_harness/CMakeLists.txt
# SUBSYSTEM-HARNESS-ARC / CLASS-LIGHT-BAND-HARNESS-1
#
# STANDALONE build (no main build64/ cache, no vcpkg, no deps):
#   cmake -S tools/class_light_band_harness -B build64-h-class_light_band_harness -A x64
#   cmake --build build64-h-class_light_band_harness --config RelWithDebInfo
#
# Links NO production .cpp, NO engine, NO GL, NO Stuff. The only production
# artefact it #includes is RenderCore/IblShCoeffs.h, which is a header-only
# constexpr float table with an explicit "no GL, no game-side includes"
# firewall comment. Everything else (shader arithmetic, the stock tgl.cpp
# kernel, the Stuff matrix math) is PORTED with the source file:line recorded
# beside each expression, exactly as tools/object_light_row_harness does.

cmake_minimum_required(VERSION 3.16)
project(class_light_band_harness CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component(MC2_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

add_executable(class_light_band_harness class_light_band_harness.cpp)
target_include_directories(class_light_band_harness PRIVATE
    ${MC2_ROOT}/tools/contract_harness_common
    ${MC2_ROOT})                       # for "RenderCore/IblShCoeffs.h"
target_compile_features(class_light_band_harness PRIVATE cxx_std_17)
