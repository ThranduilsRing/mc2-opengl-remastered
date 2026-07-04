# =============================================================================
# VK-P13b-T0  --  same-HWND VK-present-to-the-GL-window probe (standalone)
#
# Mirrors tools/vk_present_smoke, but adds opengl32 (raw GL 1.1 for the GL
# context coexistence stress) and uses raw vkCreateWin32SurfaceKHR on the GL
# window's HWND instead of SDL_Vulkan_CreateSurface.
#
# Configure + build (from this directory):
#   "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" -G "Visual Studio 17 2022" -A x64 -B build64
#   "C:/.../cmake.exe" --build build64 --config RelWithDebInfo
#
# Run (VISIBLE window, ~2 s):
#   build64/RelWithDebInfo/vk_hwnd_present_probe.exe    (exit 0 == HWND trick VIABLE)
# =============================================================================
cmake_minimum_required(VERSION 3.20)
project(vk_hwnd_present_probe CXX)

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

add_executable(vk_hwnd_present_probe main.cpp)

target_include_directories(vk_hwnd_present_probe PRIVATE
    "${MC2_3RDPARTY}/include/SDL2"
    "${VK_SDK}/Include"
)

target_link_directories(vk_hwnd_present_probe PRIVATE
    "${MC2_3RDPARTY}/lib/x64"
    "${VK_SDK}/Lib"
)

target_compile_definitions(vk_hwnd_present_probe PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)

target_link_libraries(vk_hwnd_present_probe PRIVATE
    SDL2main SDL2 vulkan-1 opengl32
)

if(MSVC)
    set_target_properties(vk_hwnd_present_probe PROPERTIES
        LINK_FLAGS "/SUBSYSTEM:CONSOLE")
endif()

add_custom_command(TARGET vk_hwnd_present_probe POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${MC2_3RDPARTY}/lib/x64/SDL2.dll" $<TARGET_FILE_DIR:vk_hwnd_present_probe>
)
