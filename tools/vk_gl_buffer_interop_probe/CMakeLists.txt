# =============================================================================
# vk_gl_buffer_interop_probe — VK-P9a WATER-PERF STEP 0 de-risk gate.
# Proves a VK-exportable BUFFER can be GL-imported (glNamedBufferStorageMemEXT),
# GL-written (compute), and VK-read GPU-side, validation-clean. Standalone.
#
# Configure + build (from this directory):
#   "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" -G "Visual Studio 17 2022" -A x64 -B build64
#   "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build build64 --config RelWithDebInfo
#   build64/RelWithDebInfo/vk_gl_buffer_interop_probe.exe   (exit 0 = PASS)
# =============================================================================
cmake_minimum_required(VERSION 3.20)
project(vk_gl_buffer_interop_probe CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(MC2_3RDPARTY "A:/Games/mc2-vk-interop-bridge/3rdparty" CACHE PATH "3rdparty dir")
if(DEFINED ENV{VULKAN_SDK})
    set(VK_SDK "$ENV{VULKAN_SDK}")
else()
    set(VK_SDK "C:/VulkanSDK/1.4.350.0")
endif()
message(STATUS "Vulkan SDK: ${VK_SDK}")

find_program(GLSLANG_VALIDATOR NAMES glslangValidator HINTS "${VK_SDK}/Bin" "${VK_SDK}/bin")
if(NOT GLSLANG_VALIDATOR)
    message(FATAL_ERROR "glslangValidator not found (looked in ${VK_SDK}/Bin). Set VULKAN_SDK.")
endif()

set(GEN_DIR "${CMAKE_BINARY_DIR}/gen")
file(MAKE_DIRECTORY "${GEN_DIR}")
function(embed_spv SRC STAGE VAR)
    set(OUT "${GEN_DIR}/${VAR}.spv.h")
    add_custom_command(OUTPUT "${OUT}"
        COMMAND "${GLSLANG_VALIDATOR}" -V -S ${STAGE} "${CMAKE_CURRENT_SOURCE_DIR}/${SRC}"
                --vn ${VAR}_spv -o "${OUT}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${SRC}" VERBATIM)
    set(EMBED_OUT_${VAR} "${OUT}" PARENT_SCOPE)
endfunction()
embed_spv(shaders/vk_read.comp comp vk_read_comp)

add_executable(vk_gl_buffer_interop_probe main.cpp "${GEN_DIR}/vk_read_comp.spv.h")
target_include_directories(vk_gl_buffer_interop_probe PRIVATE
    "${GEN_DIR}" "${MC2_3RDPARTY}/include" "${MC2_3RDPARTY}/include/SDL2" "${VK_SDK}/Include")
target_link_directories(vk_gl_buffer_interop_probe PRIVATE
    "${MC2_3RDPARTY}/lib/x64" "${VK_SDK}/Lib")
target_compile_definitions(vk_gl_buffer_interop_probe PRIVATE GLEW_DLL WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(vk_gl_buffer_interop_probe PRIVATE SDL2main SDL2 glew32 vulkan-1 opengl32)
if(MSVC)
    set_target_properties(vk_gl_buffer_interop_probe PROPERTIES LINK_FLAGS "/SUBSYSTEM:CONSOLE")
endif()
add_custom_command(TARGET vk_gl_buffer_interop_probe POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${MC2_3RDPARTY}/lib/x64/SDL2.dll"  $<TARGET_FILE_DIR:vk_gl_buffer_interop_probe>
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${MC2_3RDPARTY}/lib/x64/glew32.dll" $<TARGET_FILE_DIR:vk_gl_buffer_interop_probe>)
