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
)

target_include_directories(EditRel PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/code"
    "${CMAKE_SOURCE_DIR}/gui"
    "${CMAKE_SOURCE_DIR}/mclib"
    "${CMAKE_SOURCE_DIR}/GameOS/include"
    "${CMAKE_SOURCE_DIR}/GameOS/gameos"
    ${THIRDPARTY_INCLUDE_DIRS}
)

# --- Dependencies ---
# SDL2 is found by the root CMakeLists.txt; inherit its variables.
# GLEW, OpenGL, ZLIB also found at root scope.
find_package(SDL2 REQUIRED CONFIG REQUIRED COMPONENTS SDL2)
find_package(SDL2_mixer REQUIRED)
find_package(SDL2_ttf REQUIRED)
find_package(GLEW REQUIRED)
find_package(OpenGL REQUIRED)
find_package(ZLIB REQUIRED)

target_link_libraries(EditRel PRIVATE
    mclib
    gosfx
    mlr
    stuff
    gui
    gameos_editor

    # windows provides platform shims (string, IO, mmsystem) — no WinMain or InitGameOS.
    windows

    # OpenGL stack
    GLEW::GLEW
    OpenGL::GL

    # SDL (matches root CMakeLists conventions)
    SDL2::SDL2
    SDL2_mixer::SDL2_mixer

    # misc
    ZLIB::ZLIB
    winmm
    ws2_32
    dbghelp

    # Tracy uses Windows process inspection APIs from Psapi.lib.
    Psapi
)

add_custom_command(TARGET EditRel POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/data/defs"
        "$<TARGET_FILE_DIR:EditRel>/data/defs"
)

set_target_properties(EditRel PROPERTIES
    OUTPUT_NAME "Mission Editor"
)

add_custom_command(TARGET EditRel POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "$<TARGET_FILE_DIR:EditRel>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/data/art/editor/esplash.bmp"
        "$<TARGET_FILE_DIR:EditRel>/esplash.bmp"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/data/art/editor/tacsplash.bmp"
        "$<TARGET_FILE_DIR:EditRel>/tacsplash.bmp"
    VERBATIM
)
