# =============================================================================
# vk_interop_transport_test — OFFLINE interop-transport transparency harness
# (VK-INTEROP-BRIDGE-1, "TOOLS / offline harnesses").
#
# Standalone, self-contained: links ONLY SDL2, GLEW, vulkan-1, opengl32.
# Touches nothing in mclib / gameos / the mc2 or EditRel build. Mirrors the
# proven tools/vk_gl_interop_spike CMake pattern.
#
# Shaders (shaders/*.vert|frag) are compiled to SPIR-V and embedded as C headers
# at build time via glslangValidator --vn, so the exe has NO runtime shader-file
# dependency and is fully relocatable.
#
# Configure + build (from this directory):
#   "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" -G "Visual Studio 17 2022" -A x64 -B build64
#   "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build build64 --config RelWithDebInfo
#
# Run:
#   build64/RelWithDebInfo/vk_interop_transport_test.exe   (exit 0 = PASS)
# =============================================================================
cmake_minimum_required(VERSION 3.20)
project(vk_interop_transport_test CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(MC2_3RDPARTY "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/3rdparty"
    CACHE PATH "Path to the repo's extracted 3rdparty directory")

if(DEFINED ENV{VULKAN_SDK})
    set(VK_SDK "$ENV{VULKAN_SDK}")
else()
    set(VK_SDK "C:/VulkanSDK/1.4.350.0")
endif()
message(STATUS "Vulkan SDK: ${VK_SDK}")
message(STATUS "3rdparty  : ${MC2_3RDPARTY}")

# --- glslangValidator: compile GLSL -> embedded SPIR-V C headers --------------
find_program(GLSLANG_VALIDATOR
    NAMES glslangValidator
    HINTS "${VK_SDK}/Bin" "${VK_SDK}/bin")
if(NOT GLSLANG_VALIDATOR)
    message(FATAL_ERROR "glslangValidator not found (looked in ${VK_SDK}/Bin). Set VULKAN_SDK.")
endif()
message(STATUS "glslangValidator: ${GLSLANG_VALIDATOR}")

set(GEN_DIR "${CMAKE_BINARY_DIR}/gen")
file(MAKE_DIRECTORY "${GEN_DIR}")

# each: <glsl-src> <stage> <var-name> -> ${GEN_DIR}/<var>.spv.h  (uint32_t <var>[])
function(embed_spv SRC STAGE VAR)
    set(OUT "${GEN_DIR}/${VAR}.spv.h")
    add_custom_command(
        OUTPUT "${OUT}"
        COMMAND "${GLSLANG_VALIDATOR}" -V -S ${STAGE}
                "${CMAKE_CURRENT_SOURCE_DIR}/${SRC}"
                --vn ${VAR}_spv -o "${OUT}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${SRC}"
        COMMENT "glslang ${SRC} -> ${VAR}.spv.h"
        VERBATIM)
    set(EMBED_OUT_${VAR} "${OUT}" PARENT_SCOPE)
endfunction()

embed_spv(shaders/fullscreen.vert       vert fullscreen_vert)
embed_spv(shaders/pass_under_test.frag  frag pass_under_test_frag)

add_executable(vk_interop_transport_test
    main.cpp
    "${GEN_DIR}/fullscreen_vert.spv.h"
    "${GEN_DIR}/pass_under_test_frag.spv.h")

target_include_directories(vk_interop_transport_test PRIVATE
    "${GEN_DIR}"                        # generated *.spv.h
    "${MC2_3RDPARTY}/include"           # GL/glew.h, GL/wglew.h
    "${MC2_3RDPARTY}/include/SDL2"      # SDL.h
    "${VK_SDK}/Include")                # vulkan/vulkan.h

target_link_directories(vk_interop_transport_test PRIVATE
    "${MC2_3RDPARTY}/lib/x64"           # SDL2.lib, SDL2main.lib, glew32.lib
    "${VK_SDK}/Lib")                    # vulkan-1.lib

target_compile_definitions(vk_interop_transport_test PRIVATE GLEW_DLL WIN32_LEAN_AND_MEAN NOMINMAX)

target_link_libraries(vk_interop_transport_test PRIVATE
    SDL2main SDL2 glew32 vulkan-1 opengl32)

if(MSVC)
    set_target_properties(vk_interop_transport_test PROPERTIES
        LINK_FLAGS "/SUBSYSTEM:CONSOLE")
endif()

add_custom_command(TARGET vk_interop_transport_test POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${MC2_3RDPARTY}/lib/x64/SDL2.dll"  $<TARGET_FILE_DIR:vk_interop_transport_test>
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${MC2_3RDPARTY}/lib/x64/glew32.dll" $<TARGET_FILE_DIR:vk_interop_transport_test>)
