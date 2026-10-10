# tools/glstate_guard_harness/CMakeLists.txt
# SUBSYSTEM-HARNESS-ARC / GLSTATE-GUARD-HARNESS-1
#
# STANDALONE build (does NOT touch the main build64/ cache):
#   cmake -S tools/glstate_guard_harness -B build64-glstateguard -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH=<repo>/3rdparty
#   cmake --build build64-glstateguard --config RelWithDebInfo --target glstate_guard_harness
#   build64-glstateguard/RelWithDebInfo/glstate_guard_harness.exe --list
#
# Unlike the other contract harnesses, this one needs a REAL GL context — the
# header under test (GameOS/gameos/gl_state_guard.h) makes real
# glGetIntegerv/glEnable/glBindTexture/... calls that are meaningless without
# one. It creates a hidden SDL2 window + GL 4.3 core context (mirroring
# GameOS/gameos/gos_render.cpp's attribute set) and glewInit()s it — no game
# window, no mission data, no other GameOS/gameos production TU linked.

cmake_minimum_required(VERSION 3.16)
project(glstate_guard_harness CXX)

include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/Mc2Standalone.cmake")

get_filename_component(MC2_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

# 3rdparty root: prefer an explicit -DCMAKE_PREFIX_PATH, fall back to the
# repo's own 3rdparty/ (matches the main CMakeLists.txt convention).
if(NOT CMAKE_PREFIX_PATH)
    set(MC2_3RDPARTY "${MC2_ROOT}/3rdparty")
else()
    list(GET CMAKE_PREFIX_PATH 0 MC2_3RDPARTY)
endif()

add_executable(glstate_guard_harness glstate_guard_harness.cpp)
mc2_link_gate(glstate_guard_harness)

target_include_directories(glstate_guard_harness PRIVATE
    ${MC2_ROOT}/tools/contract_harness_common
    ${MC2_ROOT}/GameOS/gameos          # gl_state_guard.h (header-only)
    ${MC2_3RDPARTY}/include
    ${MC2_3RDPARTY}/include/SDL2
)

target_link_directories(glstate_guard_harness PRIVATE
    ${MC2_3RDPARTY}/lib/x64
)

target_link_libraries(glstate_guard_harness PRIVATE
    opengl32
    SDL2
    glew32
)

target_compile_definitions(glstate_guard_harness PRIVATE
    _CRT_SECURE_NO_WARNINGS
)

target_compile_features(glstate_guard_harness PRIVATE cxx_std_20)

# The harness dynamically links SDL2.dll/glew32.dll — copy both beside the
# .exe (mirrors the main CMakeLists.txt mc2 POST_BUILD DLL-copy pattern).
foreach(_dll_name SDL2.dll glew32.dll)
    add_custom_command(TARGET glstate_guard_harness POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${MC2_3RDPARTY}/lib/x64/${_dll_name}"
            "$<TARGET_FILE_DIR:glstate_guard_harness>/${_dll_name}"
        COMMENT "GLSTATE-GUARD-HARNESS-1: copy ${_dll_name} beside glstate_guard_harness.exe"
    )
endforeach()
