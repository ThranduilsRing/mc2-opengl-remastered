# The owned document and SDL shell have no MFC or engine allocation dependencies.
find_package(ZLIB REQUIRED)
add_library(editor_document STATIC
    document/MissionDocument.cpp
    document/MissionSyntax.cpp
    document/MissionPackets.cpp
    document/MissionStructure.cpp
    document/CommandHistory.cpp
    document/EntityCommands.cpp
)
target_compile_features(editor_document PUBLIC cxx_std_23)
target_include_directories(editor_document PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/document")
target_link_libraries(editor_document PRIVATE ZLIB::ZLIB)

function(add_editor_shell target)
    add_executable(${target} WIN32 shell/main.cpp shell/EntityPanels.cpp
        "${CMAKE_SOURCE_DIR}/3rdparty/imgui/misc/cpp/imgui_stdlib.cpp")
    add_dependencies(${target} build_fingerprint)
    target_compile_features(${target} PRIVATE cxx_std_23)
    target_compile_definitions(${target} PRIVATE SDL_MAIN_HANDLED)
    target_include_directories(${target} PRIVATE "${CMAKE_BINARY_DIR}/generated")
    target_link_libraries(${target} PRIVATE editor_document mc2_gate imgui SDL2::SDL2 OpenGL::GL)
    if(MSVC)
        target_link_options(${target} PRIVATE /ENTRY:mainCRTStartup)
    endif()
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:SDL2::SDL2>" "$<TARGET_FILE_DIR:${target}>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE_DIR:SDL2::SDL2>/zlib1.dll" "$<TARGET_FILE_DIR:${target}>"
        VERBATIM)
endfunction()

add_editor_shell(EditorShell)
set_target_properties(EditorShell PROPERTIES OUTPUT_NAME "Mission Editor Shell")
add_editor_shell(EditorShellSmoke)
set_target_properties(EditorShellSmoke PROPERTIES EXCLUDE_FROM_ALL TRUE OUTPUT_NAME "Mission Editor Smoke")
target_sources(EditorShellSmoke PRIVATE shell/PanelProof.cpp)
target_compile_definitions(EditorShellSmoke PRIVATE MC2_EDITOR_SMOKE_HOST=1)

set(editor_document_test_sources tests/test_document.cpp tests/test_structure.cpp
    "${CMAKE_SOURCE_DIR}/code/brain_missionfit_oporbd.cpp")
add_executable(editor_document_tests ${editor_document_test_sources})
target_compile_features(editor_document_tests PRIVATE cxx_std_23)
target_link_libraries(editor_document_tests PRIVATE editor_document ZLIB::ZLIB)

add_executable(editor_panel_tests tests/test_panels.cpp shell/EntityPanels.cpp shell/PanelProof.cpp
    "${CMAKE_SOURCE_DIR}/3rdparty/imgui/misc/cpp/imgui_stdlib.cpp")
target_compile_features(editor_panel_tests PRIVATE cxx_std_23)
target_compile_definitions(editor_panel_tests PRIVATE MC2_EDITOR_SMOKE_HOST=1)
target_include_directories(editor_panel_tests PRIVATE shell)
target_link_libraries(editor_panel_tests PRIVATE editor_document imgui SDL2::SDL2 OpenGL::GL)

if(MSVC)
    add_library(editor_document_asan STATIC EXCLUDE_FROM_ALL $<TARGET_PROPERTY:editor_document,SOURCES>)
    target_compile_features(editor_document_asan PUBLIC cxx_std_23)
    target_include_directories(editor_document_asan PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/document")
    target_compile_options(editor_document_asan PRIVATE /fsanitize=address)
    target_link_libraries(editor_document_asan PRIVATE ZLIB::ZLIB)
    add_executable(editor_document_tests_asan EXCLUDE_FROM_ALL ${editor_document_test_sources})
    target_compile_features(editor_document_tests_asan PRIVATE cxx_std_23)
    target_compile_options(editor_document_tests_asan PRIVATE /fsanitize=address)
    target_link_options(editor_document_tests_asan PRIVATE /INCREMENTAL:NO)
    target_link_libraries(editor_document_tests_asan PRIVATE editor_document_asan ZLIB::ZLIB)
    get_filename_component(_document_msvc_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
    add_custom_command(TARGET editor_document_tests_asan POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_document_msvc_bin}/clang_rt.asan_dynamic-x86_64.dll" "$<TARGET_FILE_DIR:editor_document_tests_asan>"
        VERBATIM
    )
endif()

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
    # gameplay_pick.cpp: shared pick spine (coord transform + glReadPixels).
    # No game-object deps; compiles cleanly against gameos + renderworld +
    # gui_runtime which the editor already links.  Needed so OnLButtonUp can
    # call tryGameplayPick for the Ctrl+Shift+LMB inspector pick.
    "${CMAKE_SOURCE_DIR}/code/gameplay_pick.cpp"
    # brain_missionfit_oporbd.cpp: pure declarative-Brain parser (no engine deps), used by
    # UnitBrainPanel's read-only "Declarative Brain" view. Compiled in directly so the editor
    # link resolves parseMissionFitBrains without depending on the game lib.
    "${CMAKE_SOURCE_DIR}/code/brain_missionfit_oporbd.cpp"
)

foreach(_editor_bridge_source IN LISTS EDITOR_BRIDGE_SOURCES)
    if(NOT EXISTS "${_editor_bridge_source}")
        message(FATAL_ERROR "Required editor bridge source is missing: ${_editor_bridge_source}")
    endif()
endforeach()

# The editor's Map Generator dialog + MissionValidator are ImGui-gated and used
# UNCONDITIONALLY in EditorInterface.cpp, so EditRel cannot build with MC2_IMGUI
# OFF. This is the silent-invalid-config trap (build64 was configured OFF and the
# editor target simply did not compile). Fail loudly at configure time instead.
if(NOT MC2_IMGUI)
    message(FATAL_ERROR
        "Editor (EditRel) requires MC2_IMGUI=ON. Re-run cmake with -DMC2_IMGUI=ON.")
endif()

add_executable(EditRel WIN32
    ${EDITOR_SOURCES}
    ${EDITOR_HEADERS}
    ${EDITOR_BRIDGE_SOURCES}
    # Real, dependency-light game TUs (no stubs needed):
    "${CMAKE_SOURCE_DIR}/code/action_executor_registry.cpp"  # theActionExecutorRegistry
    "${CMAKE_SOURCE_DIR}/code/objective_text.cpp"
    "${CMAKE_SOURCE_DIR}/code/unitprofile.cpp"               # mc2UnitProfileDataEnabled
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
    particles
    fx_trace
    rendercore
    renderworld
    gameadapters
    editorbridge   # EDITOR-BRIDGE-GPU-FIREWALL-1: editor TUs now route GPU access
                   # through EditorBridge (was direct GPU-header includes), so EditRel
                   # must link the bridge static lib that defines those symbols.

    # windows provides platform shims (string, IO, mmsystem) — no WinMain or InitGameOS.
    windows

    # OpenGL stack
    GLEW::GLEW
    OpenGL::GL

    # SDL (matches root CMakeLists conventions)
    SDL2::SDL2
    SDL2_mixer::SDL2_mixer

    # misc
    mc2_gate   # typed gate registry (Gates/); EditRel has no gameosmain, see EditorMFC.cpp
    ZLIB::ZLIB
    winmm
    ws2_32
    dbghelp
    ole32
    windowscodecs
    uuid

    # Tracy uses Windows process inspection APIs from Psapi.lib.
    Psapi
)

if(MC2_IMGUI)
    target_compile_definitions(EditRel PRIVATE MC2_IMGUI=1 MC2_IS_EDITOR=1)
    target_include_directories(EditRel PRIVATE
        "${CMAKE_SOURCE_DIR}/3rdparty/imgui"
        "${CMAKE_SOURCE_DIR}/3rdparty/imgui/backends"
        "${CMAKE_SOURCE_DIR}/GuiRuntime"
    )
    target_link_libraries(EditRel PRIVATE imgui gui_runtime)
endif()

add_custom_command(TARGET EditRel POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/data/defs"
        "$<TARGET_FILE_DIR:EditRel>/data/defs"
)

set_target_properties(EditRel PROPERTIES
    OUTPUT_NAME "Mission Editor"
)

# Runtime DLLs — must sit next to Mission Editor.exe. Without these the
# editor launches with a missing-DLL system error and the deploy script
# is the only thing currently making it runnable. CMake 3.21+ has
# TARGET_RUNTIME_DLLS, but root floor is 3.10 (see top-level
# CMakeLists.txt), so we use TARGET_FILE generator expressions for
# imported targets whose IMPORTED_LOCATION is the .dll (SDL2/SDL2_mixer/
# SDL2_ttf/mc2res) and explicit filenames for the rest (GLEW/ZLIB
# expose only the .lib via IMPORTED_LOCATION). All vcpkg DLLs are
# colocated next to SDL2.dll, so anchor on $<TARGET_FILE_DIR:SDL2::SDL2>.
set(_editor_runtime_dll_targets
    SDL2::SDL2
    SDL2_mixer::SDL2_mixer
    SDL2_ttf::SDL2_ttf
    mc2res_64
)
foreach(_t IN LISTS _editor_runtime_dll_targets)
    add_custom_command(TARGET EditRel POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:${_t}>" "$<TARGET_FILE_DIR:EditRel>"
        VERBATIM
    )
endforeach()

foreach(_dll_name glew32.dll zlib1.dll)
    add_custom_command(TARGET EditRel POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE_DIR:SDL2::SDL2>/${_dll_name}"
                "$<TARGET_FILE_DIR:EditRel>"
        VERBATIM
    )
endforeach()

# FFmpeg runtime DLLs — absolute paths resolved by the FFmpegLGPL helper
# at root scope (CMakeLists.txt:332). Editor uses the same delay-load
# pattern via shared engine code, so it needs the same DLL set.
foreach(_dll ${FFmpegLGPL_RUNTIME_DLLS})
    add_custom_command(TARGET EditRel POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_dll}" "$<TARGET_FILE_DIR:EditRel>"
        VERBATIM
    )
endforeach()

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
