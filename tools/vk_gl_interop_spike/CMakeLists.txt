# =============================================================================
# Standalone Vulkan<->OpenGL interop SPIKE  (Truth-First arc P5 gate G1)
#
# Deliberately self-contained: links ONLY SDL2, GLEW, and vulkan-1.
# Does NOT touch mclib / gameos / the mc2 or EditRel build in any way.
#
# Reuses the repo's canonical 3rdparty (SDL2 + GLEW headers/libs/dll) and the
# installed Vulkan SDK (headers + vulkan-1.lib). Paths are explicit so this
# configures with a bare `cmake -B build64` and no CMAKE_PREFIX_PATH needed.
#
# Configure + build (from this directory):
#   "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" -G "Visual Studio 17 2022" -A x64 -B build64
#   "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build build64 --config RelWithDebInfo
#
# Run:
#   VK_LAYER_KHRONOS_validation is auto-enabled if the SDK layers are on PATH
#   (they are, via the installed Vulkan SDK). Then:
#   build64/RelWithDebInfo/vk_gl_interop_spike.exe
# =============================================================================
cmake_minimum_required(VERSION 3.20)
project(vk_gl_interop_spike CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- canonical repo 3rdparty (has SDL2 + GLEW headers/libs/dll) --------------
# The agent worktree's own 3rdparty is only partially extracted, so point at the
# fully-populated nifty-mendeleev worktree. Override with -DMC2_3RDPARTY=... .
set(MC2_3RDPARTY "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/3rdparty"
    CACHE PATH "Path to the repo's extracted 3rdparty directory")

# --- Vulkan SDK --------------------------------------------------------------
# Prefer $ENV{VULKAN_SDK}; fall back to the known install.
if(DEFINED ENV{VULKAN_SDK})
    set(VK_SDK "$ENV{VULKAN_SDK}")
else()
    set(VK_SDK "C:/VulkanSDK/1.4.350.0")
endif()
message(STATUS "Vulkan SDK: ${VK_SDK}")
message(STATUS "3rdparty  : ${MC2_3RDPARTY}")

add_executable(vk_gl_interop_spike main.cpp)

target_include_directories(vk_gl_interop_spike PRIVATE
    "${MC2_3RDPARTY}/include"          # GL/glew.h, GL/wglew.h
    "${MC2_3RDPARTY}/include/SDL2"     # SDL.h
    "${VK_SDK}/Include"                # vulkan/vulkan.h
)

target_link_directories(vk_gl_interop_spike PRIVATE
    "${MC2_3RDPARTY}/lib/x64"          # SDL2.lib, SDL2main.lib, glew32.lib
    "${VK_SDK}/Lib"                    # vulkan-1.lib
)

# GLEW is linked as the import lib for glew32.dll (NOT glew32s static),
# so define GLEW_DLL. opengl32 for the core GL entry points.
target_compile_definitions(vk_gl_interop_spike PRIVATE GLEW_DLL WIN32_LEAN_AND_MEAN NOMINMAX)

target_link_libraries(vk_gl_interop_spike PRIVATE
    SDL2main SDL2 glew32 vulkan-1 opengl32
)

# SDL2 wants /SUBSYSTEM:CONSOLE with a plain int main via SDL2main.
if(MSVC)
    set_target_properties(vk_gl_interop_spike PROPERTIES
        LINK_FLAGS "/SUBSYSTEM:CONSOLE")
endif()

# --- copy the runtime DLLs next to the exe so it runs in-place ---------------
add_custom_command(TARGET vk_gl_interop_spike POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${MC2_3RDPARTY}/lib/x64/SDL2.dll"  $<TARGET_FILE_DIR:vk_gl_interop_spike>
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${MC2_3RDPARTY}/lib/x64/glew32.dll" $<TARGET_FILE_DIR:vk_gl_interop_spike>
)

# =============================================================================
# T0 depth-export micro-spike (VK-INTEROP-BRIDGE-1) — SEPARATE target.
# Extends the color spike to prove D32_SFLOAT SAMPLED zero-copy VK->GL interop.
# Same standalone deps; the proven color spike above is untouched.
# =============================================================================
add_executable(vk_gl_interop_depth_spike main_depth.cpp)

target_include_directories(vk_gl_interop_depth_spike PRIVATE
    "${MC2_3RDPARTY}/include"
    "${MC2_3RDPARTY}/include/SDL2"
    "${VK_SDK}/Include"
)
target_link_directories(vk_gl_interop_depth_spike PRIVATE
    "${MC2_3RDPARTY}/lib/x64"
    "${VK_SDK}/Lib"
)
target_compile_definitions(vk_gl_interop_depth_spike PRIVATE GLEW_DLL WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(vk_gl_interop_depth_spike PRIVATE
    SDL2main SDL2 glew32 vulkan-1 opengl32
)
if(MSVC)
    set_target_properties(vk_gl_interop_depth_spike PROPERTIES
        LINK_FLAGS "/SUBSYSTEM:CONSOLE")
endif()
add_custom_command(TARGET vk_gl_interop_depth_spike POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${MC2_3RDPARTY}/lib/x64/SDL2.dll"  $<TARGET_FILE_DIR:vk_gl_interop_depth_spike>
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${MC2_3RDPARTY}/lib/x64/glew32.dll" $<TARGET_FILE_DIR:vk_gl_interop_depth_spike>
)

# =============================================================================
# P5-T0' packed depth-stencil micro-spike (VK-INTEROP-BRIDGE-1) — SEPARATE target.
# The GO/NO-GO for P5 (postprocess depth zero-copy). Extends the depth spike to
# the engine's REAL scene-depth boundary: VK_FORMAT_D24_UNORM_S8_UINT (fallback
# D32_SFLOAT_S8_UINT) shared as a GL_DEPTH24_STENCIL8 texture. Standalone deps.
# =============================================================================
add_executable(vk_gl_interop_d24s8_spike main_d24s8.cpp)

target_include_directories(vk_gl_interop_d24s8_spike PRIVATE
    "${MC2_3RDPARTY}/include"
    "${MC2_3RDPARTY}/include/SDL2"
    "${VK_SDK}/Include"
)
target_link_directories(vk_gl_interop_d24s8_spike PRIVATE
    "${MC2_3RDPARTY}/lib/x64"
    "${VK_SDK}/Lib"
)
target_compile_definitions(vk_gl_interop_d24s8_spike PRIVATE GLEW_DLL WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(vk_gl_interop_d24s8_spike PRIVATE
    SDL2main SDL2 glew32 vulkan-1 opengl32
)
if(MSVC)
    set_target_properties(vk_gl_interop_d24s8_spike PROPERTIES
        LINK_FLAGS "/SUBSYSTEM:CONSOLE")
endif()
add_custom_command(TARGET vk_gl_interop_d24s8_spike POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${MC2_3RDPARTY}/lib/x64/SDL2.dll"  $<TARGET_FILE_DIR:vk_gl_interop_d24s8_spike>
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${MC2_3RDPARTY}/lib/x64/glew32.dll" $<TARGET_FILE_DIR:vk_gl_interop_d24s8_spike>
)
