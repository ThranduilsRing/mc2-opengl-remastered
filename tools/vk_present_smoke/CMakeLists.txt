# =============================================================================
# Standalone headless VK swapchain/present + fallback SMOKE harness
#   (VK-P13-SELECTOR-PLAN T0 -- flips the Tooling-Ledger "present/window" row)
#
# Deliberately self-contained: links ONLY SDL2 + vulkan-1 (raw vulkan.h). Does
# NOT touch mclib / gameos / the mc2 or EditRel build in any way -- it MIRRORS
# the L5 present object's swapchain/acquire/present/resize logic rather than
# linking it (the present object is volk+vk-bootstrap + engine-coupled).
#
# No GL side (unlike vk_gl_interop_spike): a pure VK-present smoke fills a
# VK-owned image and blits it to the swapchain, so no GLEW/opengl32 dependency.
#
# Configure + build (from this directory):
#   "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" -G "Visual Studio 17 2022" -A x64 -B build64
#   "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build build64 --config RelWithDebInfo
#
# Run:
#   build64/RelWithDebInfo/vk_present_smoke.exe          (exit 0 == PASS)
#   MC2_VK_NEG_TEST=skipblit  build64/.../vk_present_smoke.exe   (negative test)
# =============================================================================
cmake_minimum_required(VERSION 3.20)
project(vk_present_smoke CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- canonical repo 3rdparty (has SDL2 headers/libs/dll) ---------------------
# The agent worktree's own 3rdparty is only partially extracted, so point at the
# fully-populated nifty-mendeleev worktree. Override with -DMC2_3RDPARTY=... .
set(MC2_3RDPARTY "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/3rdparty"
    CACHE PATH "Path to the repo's extracted 3rdparty directory")

# --- Vulkan SDK --------------------------------------------------------------
if(DEFINED ENV{VULKAN_SDK})
    set(VK_SDK "$ENV{VULKAN_SDK}")
else()
    set(VK_SDK "C:/VulkanSDK/1.4.350.0")
endif()
message(STATUS "Vulkan SDK: ${VK_SDK}")
message(STATUS "3rdparty  : ${MC2_3RDPARTY}")

add_executable(vk_present_smoke main.cpp)

target_include_directories(vk_present_smoke PRIVATE
    "${MC2_3RDPARTY}/include/SDL2"     # SDL.h, SDL_vulkan.h
    "${VK_SDK}/Include"                # vulkan/vulkan.h
)

target_link_directories(vk_present_smoke PRIVATE
    "${MC2_3RDPARTY}/lib/x64"          # SDL2.lib, SDL2main.lib
    "${VK_SDK}/Lib"                    # vulkan-1.lib
)

target_compile_definitions(vk_present_smoke PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)

target_link_libraries(vk_present_smoke PRIVATE
    SDL2main SDL2 vulkan-1
)

# SDL2 wants /SUBSYSTEM:CONSOLE with a plain int main via SDL2main.
if(MSVC)
    set_target_properties(vk_present_smoke PROPERTIES
        LINK_FLAGS "/SUBSYSTEM:CONSOLE")
endif()

# --- copy the runtime DLL next to the exe so it runs in-place ----------------
add_custom_command(TARGET vk_present_smoke POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${MC2_3RDPARTY}/lib/x64/SDL2.dll" $<TARGET_FILE_DIR:vk_present_smoke>
)
