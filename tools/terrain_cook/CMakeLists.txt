cmake_minimum_required(VERSION 3.16)
project(mc2_terrain_cook CXX)
get_filename_component(MC2_COOK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    include("${MC2_COOK_ROOT}/cmake/Mc2Standalone.cmake")
endif()
add_executable(terrain_cook main.cpp
    "${MC2_COOK_ROOT}/GameOS/gameos/cook_cache.cpp"
    "${MC2_COOK_ROOT}/GameOS/gameos/terrain_surface_asset.cpp"
    "${MC2_COOK_ROOT}/GameOS/gameos/terrain_surface_cook.cpp")
target_include_directories(terrain_cook PRIVATE "${MC2_COOK_ROOT}/GameOS/gameos")
target_compile_features(terrain_cook PRIVATE cxx_std_23)
