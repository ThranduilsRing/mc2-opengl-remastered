# EditRel / MC2 Mission Editor port staging target.
# Windows-only for now because the original editor is MFC-based.

if(NOT MSVC)
    message(STATUS "EditRel editor target skipped: original editor is MFC/Windows-only at this stage.")
    return()
endif()

file(GLOB EDITOR_SOURCES CONFIGURE_DEPENDS
    "*.cpp"
    "*.c"
    "*.rc"
)

file(GLOB EDITOR_HEADERS CONFIGURE_DEPENDS
    "*.h"
    "*.hpp"
)

set(EDITOR_BRIDGE_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/EditorGosRender.cpp"
)

foreach(_editor_bridge_source IN LISTS EDITOR_BRIDGE_SOURCES)
    if(NOT EXISTS "${_editor_bridge_source}")
        message(FATAL_ERROR "Required editor bridge source is missing: ${_editor_bridge_source}")
    endif()
endforeach()


add_executable(EditRel WIN32
    ${EDITOR_SOURCES}
    ${EDITOR_HEADERS}
    ${EDITOR_BRIDGE_SOURCES}
)

# MFC setup
set_target_properties(EditRel PROPERTIES
    VS_GLOBAL_UseOfMfc "Dynamic"
)

# Force the MFC editor target to use the normal WinMain entry point.
# Do not link SDL2main for EditRel; SDL is initialized from the MFC-owned startup path.
target_link_options(EditRel PRIVATE /ENTRY:WinMainCRTStartup)

target_compile_definitions(EditRel PRIVATE
    _AFXDLL
    PLATFORM_WINDOWS
    _CRT_SECURE_NO_WARNINGS
    LINUX_BUILD=0
    # Suppress the main() / SDL_main definition inside gameosmain.cpp.
    # gameosmain.cpp guards its entry point with #ifndef DISABLE_GAMEOS_MAIN.
    # The editor owns its own entry point (MFC AfxWinMain via /ENTRY:WinMainCRTStartup);
    # the gameosmain main() was the hidden caller of GetGameOSEnvironment that ran
    # before MFC InitInstance and made SDL_CreateWindow crash pre-pump.
    DISABLE_GAMEOS_MAIN
)

target_include_directories(EditRel PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/code"
    "${CMAKE_SOURCE_DIR}/gui"
    "${CMAKE_SOURCE_DIR}/mclib"
    "${CMAKE_SOURCE_DIR}/GameOS/include"
    "${CMAKE_SOURCE_DIR}/GameOS/gameos"
)

# --- Dependencies (vcpkg-managed) ---
find_package(SDL2 CONFIG REQUIRED)
find_package(SDL2_mixer CONFIG REQUIRED)
find_package(SDL2_ttf CONFIG REQUIRED)
find_package(GLEW REQUIRED)
find_package(OpenGL REQUIRED)
find_package(ZLIB REQUIRED)

target_link_libraries(EditRel PRIVATE
    mclib
    gosfx
    mlr
    stuff
    gui
    gameos
    # gameos_main is linked for completeness (crash bundle, smoke mode stubs, etc.)
    # but its main() / SDL_main entry point is suppressed by DISABLE_GAMEOS_MAIN above.
    # Without that define, gameosmain.cpp's main() calls GetGameOSEnvironment at process
    # start before MFC InitInstance, which triggered SDL_CreateWindow before the Win32
    # message pump was live — causing the startup crash.
    gameos_main
    # windows provides platform shims (string, IO, mmsystem) — no WinMain or InitGameOS.
    windows

    # OpenGL stack
    GLEW::GLEW
    OpenGL::GL

    # SDL stack (vcpkg)
    SDL2::SDL2
    SDL2_mixer::SDL2_mixer
    SDL2_ttf::SDL2_ttf

    # misc
    ZLIB::ZLIB
    winmm

    # --- CRITICAL FIX ---
    # Tracy uses Windows process inspection APIs from Psapi.lib
    Psapi
)