# tools/class_light_band_harness/CMakeLists.txt
# SUBSYSTEM-HARNESS-ARC / CLASS-LIGHT-BAND-HARNESS-1
#
# STANDALONE build (no main build64/ cache, no vcpkg, no deps):
#   cmake -S tools/class_light_band_harness -B build64-h-class_light_band_harness -A x64
#   cmake --build build64-h-class_light_band_harness --config RelWithDebInfo
#
# C++20 because the header-only Gates/render_options.h (shipped option defaults and value grammar) is.
# Links NO production .cpp, NO engine, NO GL, NO Stuff. The only production
# artefacts it #includes are RenderCore/IblShCoeffs.h and (Stage F)
# RenderCore/IblShRegistry.h -- both header-only constexpr tables carrying an
# explicit "no GL, no game-side includes" firewall comment. Stage F reads the
# registry directly so that "kMissionShMap is empty" is asserted against the
# TABLE rather than against a comment about it. Everything else (shader
# arithmetic, the stock tgl.cpp
# kernel, the Stuff matrix math) is PORTED with the source file:line recorded
# beside each expression, exactly as tools/object_light_row_harness does.

cmake_minimum_required(VERSION 3.16)
project(class_light_band_harness CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component(MC2_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

add_executable(class_light_band_harness class_light_band_harness.cpp)
target_include_directories(class_light_band_harness PRIVATE
    ${MC2_ROOT}/tools/contract_harness_common
    ${MC2_ROOT})                       # for "RenderCore/IblShCoeffs.h"
target_compile_features(class_light_band_harness PRIVATE cxx_std_20)   # Gates/render_options.h (the live option defaults) is C++20
