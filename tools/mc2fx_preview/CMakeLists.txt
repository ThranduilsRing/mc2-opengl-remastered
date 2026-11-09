# tools/mc2fx_preview — GUI curve previewer for gosFX .fx blobs.
#
# Loads an .fx blob headless (reuses tools/mc2fx/mc2fx_core.cpp), lists effects,
# and PLOTS each selected effect's animation curves vs particle age (0..1) with
# Dear ImGui (SDL2 + Vulkan). This is a curve grapher, NOT a particle renderer.
#
# Build with -DENABLE_MC2FX_PREVIEW=ON (guarded in the top CMakeLists).
#
# ISOLATION: the gosFX/MLR/particles/Stuff closure + mc2fx_stubs.cpp need
# PLATFORM_WINDOWS + LINUX_BUILD and define gos_* symbols. They go in their own
# STATIC lib (mc2fx_corelib) with those defines, exactly like asset_viewer
# isolates mc2_tglloader — so the defines never leak onto the imgui/SDL TUs.

# ---------------------------------------------------------------------------
# mc2fx_corelib — gosFX closure + core load/sample API + stubs (isolated).
# ---------------------------------------------------------------------------
file(GLOB MC2FXP_GOSFX_SRC     "${CMAKE_SOURCE_DIR}/mclib/gosfx/*.cpp")
file(GLOB MC2FXP_MLR_SRC       "${CMAKE_SOURCE_DIR}/mclib/mlr/*.cpp")
file(GLOB MC2FXP_PARTICLES_SRC "${CMAKE_SOURCE_DIR}/mclib/particles/*.cpp")
file(GLOB MC2FXP_STUFF_SRC     "${CMAKE_SOURCE_DIR}/mclib/stuff/*.cpp")


add_library(mc2fx_corelib STATIC
    "${CMAKE_CURRENT_SOURCE_DIR}/../mc2fx/mc2fx_core.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../mc2fx/mc2fx_stubs.cpp"
    ${MC2FXP_GOSFX_SRC}
    ${MC2FXP_MLR_SRC}
    ${MC2FXP_PARTICLES_SRC}
    "${CMAKE_SOURCE_DIR}/GameOS/gameos/sim_overlap.cpp"
    "${CMAKE_SOURCE_DIR}/code/render_view_store.cpp"
    ${MC2FXP_STUFF_SRC}
    "${CMAKE_SOURCE_DIR}/3rdparty/tracy/TracyClient.cpp"
)

target_compile_features(mc2fx_corelib PRIVATE cxx_std_17)
target_compile_definitions(mc2fx_corelib PRIVATE
    PLATFORM_WINDOWS
    LINUX_BUILD
    _CRT_SECURE_NO_WARNINGS
)
target_include_directories(mc2fx_corelib PRIVATE
    "${CMAKE_SOURCE_DIR}/mclib"
    "${CMAKE_SOURCE_DIR}/mclib/stuff"
    "${CMAKE_SOURCE_DIR}/mclib/gosfx"
    "${CMAKE_SOURCE_DIR}/mclib/mlr"
    "${CMAKE_SOURCE_DIR}/mclib/particles"
    "${CMAKE_SOURCE_DIR}/GameOS/include"
    "${CMAKE_SOURCE_DIR}/GameOS/gameos"
    "${CMAKE_SOURCE_DIR}/3rdparty/include"
    "${CMAKE_SOURCE_DIR}/3rdparty/tracy"
    "${CMAKE_SOURCE_DIR}"
)
# mc2fx_core.h is the only public surface the previewer needs.
target_include_directories(mc2fx_corelib PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/../mc2fx"
)

find_library(MC2FXP_ZLIB
    NAMES zlib zlibstatic
    PATHS
        "${CMAKE_SOURCE_DIR}/3rdparty/3rdparty/lib/x64"
        "${CMAKE_SOURCE_DIR}/3rdparty/lib/x64"
        "A:/Games/mc2-opengl-src/3rdparty/3rdparty/lib/x64"
    NO_DEFAULT_PATH
)
if(MC2FXP_ZLIB)
    target_link_libraries(mc2fx_corelib PRIVATE ${MC2FXP_ZLIB})
endif()
if(WIN32)
    target_link_libraries(mc2fx_corelib PRIVATE Psapi)  # Tracy
endif()

# The gosFX / mlr / particles TUs read their gates through the typed registry.
target_link_libraries(mc2fx_corelib PRIVATE mc2_gate fx_native)

# ---------------------------------------------------------------------------
# mc2fx_preview — SDL2 + Vulkan + Dear ImGui shell (curve grapher).
# ---------------------------------------------------------------------------
add_executable(mc2fx_preview
    main.cpp
)
target_compile_features(mc2fx_preview PRIVATE cxx_std_17)
target_compile_definitions(mc2fx_preview PRIVATE)

if(NOT TARGET SDL2::SDL2)
    find_package(SDL2 CONFIG REQUIRED)
endif()

# SDL include discovery (copied from tools/asset_viewer).
set(MC2FXP_SDL_INCLUDE_DIRS)
if(DEFINED SDL2_INCLUDE_DIRS)
    list(APPEND MC2FXP_SDL_INCLUDE_DIRS ${SDL2_INCLUDE_DIRS})
endif()
if(DEFINED SDL2_INCLUDE_DIR)
    list(APPEND MC2FXP_SDL_INCLUDE_DIRS ${SDL2_INCLUDE_DIR})
endif()
foreach(_p IN LISTS CMAKE_PREFIX_PATH)
    if(EXISTS "${_p}/include/SDL2/SDL.h")
        list(APPEND MC2FXP_SDL_INCLUDE_DIRS "${_p}/include/SDL2")
    endif()
    if(EXISTS "${_p}/include/SDL.h")
        list(APPEND MC2FXP_SDL_INCLUDE_DIRS "${_p}/include")
    endif()
endforeach()
foreach(_vr
    "${CMAKE_BINARY_DIR}/vcpkg_installed/x64-windows"
    "${CMAKE_BINARY_DIR}/vcpkg_installed/x86-windows"
    "${CMAKE_BINARY_DIR}/vcpkg_installed/x64-linux"
)
    if(EXISTS "${_vr}/include/SDL2/SDL.h")
        list(APPEND MC2FXP_SDL_INCLUDE_DIRS "${_vr}/include/SDL2")
    endif()
    if(EXISTS "${_vr}/include/SDL.h")
        list(APPEND MC2FXP_SDL_INCLUDE_DIRS "${_vr}/include")
    endif()
endforeach()
if(MC2FXP_SDL_INCLUDE_DIRS)
    list(REMOVE_DUPLICATES MC2FXP_SDL_INCLUDE_DIRS)
endif()

target_include_directories(mc2fx_preview PRIVATE
    "${CMAKE_SOURCE_DIR}/3rdparty/imgui"
    "${CMAKE_SOURCE_DIR}/3rdparty/imgui/backends"
    "${CMAKE_SOURCE_DIR}/3rdparty/include"
    ${MC2FXP_SDL_INCLUDE_DIRS}
)

target_link_libraries(mc2fx_preview PRIVATE mc2fx_corelib imgui)

if(TARGET SDL2::SDL2)
    target_link_libraries(mc2fx_preview PRIVATE SDL2::SDL2)
elseif(DEFINED SDL2_LIBRARIES)
    target_link_libraries(mc2fx_preview PRIVATE ${SDL2_LIBRARIES})
else()
    message(FATAL_ERROR "SDL2 target or SDL2_LIBRARIES not found for mc2fx_preview")
endif()
if(TARGET SDL2::SDL2main)
    target_link_libraries(mc2fx_preview PRIVATE SDL2::SDL2main)
endif()

# Vulkan shell shared by the viewport tools (tools/common). It carries imgui + SDL2 + volk.
if(NOT TARGET mc2_tool_vk)
    add_subdirectory("${CMAKE_SOURCE_DIR}/tools/common" "${CMAKE_BINARY_DIR}/tools_common")
endif()
target_link_libraries(mc2fx_preview PRIVATE mc2_tool_vk)
if(WIN32)
    target_link_libraries(mc2fx_preview PRIVATE Psapi)
endif()
