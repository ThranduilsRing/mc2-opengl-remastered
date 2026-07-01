cmake_minimum_required (VERSION 3.10)
# select GLVND versus legacy OpenGL libraries
cmake_policy(SET CMP0072 NEW) # cmake --help-policy CMP0072
project(mc2)

# C++17 baseline for the whole project (CXX17-1 flip 2026-05-24).
# RenderCore/RenderWorld/GameAdapters/tests already require cxx_std_17 via
# target_compile_features; flipping the root removes the C++14/17 mix and
# makes mc2.exe uniformly C++17. See docs/superpowers/explorations/
# 2026-05-24-cxx17-upgrade-recon.md.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CMakeToolsHelpers OPTIONAL)

# Set a default build type for single-configuration
# CMake generators if no build type is set.
#IF(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
#   SET(CMAKE_BUILD_TYPE RelWithDebInfo)
#ENDIF(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)

message("CMAKE library architecture: ${CMAKE_LIBRARY_ARCHITECTURE}")

if("${CMAKE_SIZEOF_VOID_P}" EQUAL "8")
	message(STATUS "Target is 64 bits")
	set_property(GLOBAL PROPERTY FIND_LIBRARY_USE_LIB64_PATHS TRUE)
else("${CMAKE_SIZEOF_VOID_P}" EQUAL "8")
	message(STATUS "Target is 32 bits")
endif("${CMAKE_SIZEOF_VOID_P}" EQUAL "8")

set(CMAKE_EXPORT_COMPILE_COMMANDS "ON")

# Normalize CMake module paths before they are inherited by try_compile()
# projects. Raw Windows backslashes in generated scratch CMakeLists files can
# be parsed as escapes, e.g. D:\Repos -> invalid "\R".
file(TO_CMAKE_PATH "${CMAKE_CURRENT_SOURCE_DIR}" MC2_SOURCE_DIR_CMAKE)

foreach(MC2_PREFIX_PATH_ENTRY IN LISTS CMAKE_PREFIX_PATH)
    if(NOT "${MC2_PREFIX_PATH_ENTRY}" STREQUAL "")
        file(TO_CMAKE_PATH "${MC2_PREFIX_PATH_ENTRY}" MC2_PREFIX_PATH_ENTRY_CMAKE)
        list(APPEND CMAKE_MODULE_PATH "${MC2_PREFIX_PATH_ENTRY_CMAKE}/cmake")
    endif()
endforeach()

list(APPEND CMAKE_MODULE_PATH
    "${MC2_SOURCE_DIR_CMAKE}/3rdparty/cmake"
    "${MC2_SOURCE_DIR_CMAKE}/cmake/sdl2"
)
# may have to help find_package bydetting path to FindSDL2.cmake
#set(SDL2_DIR "${CMAKE_PREFIX_PATH}/cmake")

if(NOT MSVC)
    # CXX17-1 2026-05-24: removed dead -std=c++0x (project standard now set via
    # CMAKE_CXX_STANDARD 17 above; this branch is never reached on Windows builds).
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -ggdb")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-unused-local-typedefs -Werror=array-bounds -Wno-switch -Wwrite-strings -Wno-unknown-warning-option -Wunused-variable -fpermissive") # -fsanitize=address -fno-omit-frame-pointer")
endif()
# -Wno-unused-local-typedefs - to disable "typedef was ignored on this declaration"

if(MSVC)
	add_definitions(-DPLATFORM_WINDOWS)
	add_definitions(-D_CRT_SECURE_NO_WARNINGS)
	message("Compiling on M$ Windows")
endif()

# to support windows sln file generator as well
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -D_ARMOR -D_DEBUG -DBUGLOG -DLAB_ONLY")
#set(CMAKE_CXX_FLAGS_RELWITHDEBINFO" ${CMAKE_CXX_FLAGS_RELWITHDEBINFO} -DSOME_STUFF")
if(MSVC)
    set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} /MTd /MP")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /MP")
    #set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /MP -DWINDOWS_IGNORE_PACKING_MISMATCH")
endif()

# ---------------------------------------------------------------------------
# MC2_ASAN: parallel dev build with MSVC AddressSanitizer.
#
# Usage:
#   cmake -S . -B build64-asan -A x64 -DMC2_ASAN=ON
#   cmake --build build64-asan --config RelWithDebInfo
#
# What this does:
#   * Adds /fsanitize=address and disables flags that are incompatible with
#     ASan: /RTC1 (runtime checks), /INCREMENTAL (incremental link), and
#     /MP (parallel compile, known to tickle ASan linker state on 17.x).
#   * Disables Tracy. Tracy's allocator hooks race with ASan's interceptors
#     and you cannot profile with ASan anyway.
#   * Copies clang_rt.asan_dynamic-x86_64.dll next to mc2.exe at POST_BUILD.
#     The exe will not launch without it.
#
# Run with:
#   set ASAN_OPTIONS=detect_leaks=0:abort_on_error=1:halt_on_error=0:print_stacktrace=1:symbolize=1
#   mc2.exe -mission mc2_01
#
# Notes:
#   * gos_Malloc/gos_Free are already plain malloc/free (see GameOS/gameos/
#     gameos.cpp:245). So ASan sees every new/delete with zero bypass work.
#   * The only custom slab remaining is TG_GOSVertexPool (mclib/tgl.cpp).
#     Left alone for MVP; annotate with __asan_poison_memory_region later
#     if ASan stays quiet but pool-exhaustion events keep firing.
# ---------------------------------------------------------------------------
option(MC2_ASAN "Build with MSVC AddressSanitizer (parallel dev build)" OFF)
option(MC2_IMGUI "Enable in-game ImGui UI" OFF)
# DETERMINISTIC-RNG-1: compile-in the per-gos_rand() "RNG" JSONL trace. Default OFF
# so stock builds pay nothing; runtime still requires MC2_DETERMINISTIC_RNG=1 +
# MC2_DIAG_TAGS=RNG. Turn ON only for determinism-proof builds.
option(MC2_RNG_TRACE "Compile the gos_rand RNG diagnostic trace (DETERMINISTIC-RNG-1)" OFF)
# VULKAN-BACKEND-SKELETON-1: compile-gate the Vulkan bootstrap skeleton TU.
# Default OFF -> zero Vulkan code compiled, zero Vulkan headers/loader required,
# normal build completely unaffected + no Vulkan dependency. When ON, we
# find_package(Vulkan) (the SDK loader/headers), link it into gameos, and
# compile GameOS/gameos/vulkan_backend_skeleton.cpp under -DMC2_VULKAN.
option(MC2_VULKAN "Compile the Vulkan bootstrap skeleton (VULKAN-BACKEND-SKELETON-1)" OFF)
if(MC2_VULKAN)
    find_package(Vulkan REQUIRED)
    message(STATUS "MC2_VULKAN=ON -- Vulkan found: ${Vulkan_LIBRARY} (include ${Vulkan_INCLUDE_DIR})")
endif()

# VULKAN-SHADER-TOOLCHAIN-1: standalone headless probe harness (ON only).
# Links ONLY the Vulkan loader + the skeleton TU -- no GL/SDL/window/game.
# Verifies the SPIR-V shader-module load path against the compiled .spv.
# OFF -> target does not exist.
if(MC2_VULKAN)
    add_executable(vulkan_shader_probe
        tools/vulkan_shader_probe_main.cpp
        GameOS/gameos/vulkan_backend_skeleton.cpp)
    target_include_directories(vulkan_shader_probe PRIVATE GameOS/gameos)
    target_compile_definitions(vulkan_shader_probe PRIVATE MC2_VULKAN)
    target_link_libraries(vulkan_shader_probe PRIVATE Vulkan::Vulkan)
    if(TARGET mc2_vulkan_shaders)
        add_dependencies(vulkan_shader_probe mc2_vulkan_shaders)
    endif()
endif()

if(MC2_ASAN)
    if(NOT MSVC)
        message(FATAL_ERROR "MC2_ASAN is only wired for MSVC. Use -fsanitize=address manually on other toolchains.")
    endif()
    message(STATUS "MC2_ASAN=ON -- building with /fsanitize=address; Tracy disabled")
    add_definitions(-DMC2_ASAN)

    # Strip /MP from the global CXX flags (incompatible with ASan on some 17.x).
    string(REPLACE "/MP" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
    string(REPLACE "/MP" "" CMAKE_C_FLAGS   "${CMAKE_C_FLAGS}")
    string(REPLACE "/MP" "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")

    # Strip /RTC1 from the default Debug flag sets (ASan refuses to link with it).
    foreach(_cfg CXX_FLAGS_DEBUG C_FLAGS_DEBUG CXX_FLAGS_RELWITHDEBINFO C_FLAGS_RELWITHDEBINFO)
        string(REGEX REPLACE "/RTC[1csu]+" "" CMAKE_${_cfg} "${CMAKE_${_cfg}}")
    endforeach()

    # Add the sanitizer flag globally.
    set(CMAKE_C_FLAGS   "${CMAKE_C_FLAGS}   /fsanitize=address")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /fsanitize=address")

    # Disable incremental linking for all exe/shared targets.
    foreach(_cfg "" "_DEBUG" "_RELWITHDEBINFO" "_RELEASE" "_MINSIZEREL")
        string(REPLACE "/INCREMENTAL" "/INCREMENTAL:NO"
               CMAKE_EXE_LINKER_FLAGS${_cfg} "${CMAKE_EXE_LINKER_FLAGS${_cfg}}")
        string(REPLACE "/INCREMENTAL" "/INCREMENTAL:NO"
               CMAKE_SHARED_LINKER_FLAGS${_cfg} "${CMAKE_SHARED_LINKER_FLAGS${_cfg}}")
    endforeach()
endif()

add_definitions(-DUSE_ASSEMBLER_CODE=0)
add_definitions(-DLINUX_BUILD)

if(NOT MC2_ASAN)
    # Tracy profiler (always-on, ~1ns overhead when not connected)
    add_definitions(-DTRACY_ENABLE)
    # On-demand mode: only enqueue events while a profiler is actively connected.
    # Without this, the client's in-process event queue grows unbounded until a profiler
    # attaches, which looks like a memory leak that "stops" when Tracy is launched.
    add_definitions(-DTRACY_ON_DEMAND)
    # Disable ETW kernel sampling (requires elevated privileges, not needed for zone profiling)
    add_definitions(-DTRACY_NO_SYSTEM_TRACING)
endif()

find_package(SDL2 REQUIRED CONFIG REQUIRED COMPONENTS SDL2)
find_package(SDL2 REQUIRED CONFIG REQUIRED COMPONENTS SDL2main)
find_package(SDL2_mixer REQUIRED)
#required by text_tool
find_package(SDL2_ttf REQUIRED)
find_package(GLEW REQUIRED)
find_package(OpenGL REQUIRED)
find_package(ZLIB REQUIRED)

list(APPEND CMAKE_MODULE_PATH "${MC2_SOURCE_DIR_CMAKE}/cmake")
find_package(FFmpegLGPL REQUIRED)
# Exposes MC2_VIDEO_DLL_LIST (C string array) in ${CMAKE_BINARY_DIR}/generated/mc2video_dlls.h
include_directories("${CMAKE_BINARY_DIR}/generated")

message("Found SDL2 package ${SDL2_LIBRARIES}" )
message("SDL2 prefix: ${SDL2_PREFIX}")
message("OPENGL library found in ${OPENGL}")
message("GLEW library found in ${GLEW}")
message("Zlib library found in ${ZLIB}")

message("OpenGL library linking: ${OPENGL_gl_LIBRARY}")
message("OpenGL library include: ${OPENGL_INCLUDE_DIR}")


get_filename_component(COM_PATH1 "./GameOS/include" ABSOLUTE)
get_filename_component(COM_PATH2 "./GameOS/gameos" ABSOLUTE)

set(COMMON_INCLUDE_DIRS  ${COM_PATH1} ${COM_PATH2})
set(THIRDPARTY_INCLUDE_DIRS "")
foreach(MC2_PREFIX_PATH_ENTRY IN LISTS CMAKE_PREFIX_PATH)
    if(NOT "${MC2_PREFIX_PATH_ENTRY}" STREQUAL "")
        file(TO_CMAKE_PATH "${MC2_PREFIX_PATH_ENTRY}" MC2_PREFIX_PATH_ENTRY_CMAKE)
        list(APPEND THIRDPARTY_INCLUDE_DIRS "${MC2_PREFIX_PATH_ENTRY_CMAKE}/include")
    endif()
endforeach()
list(APPEND THIRDPARTY_INCLUDE_DIRS "${MC2_SOURCE_DIR_CMAKE}/3rdparty/include")
# Tracy headers resolve as <tracy/Tracy.hpp>
list(APPEND THIRDPARTY_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/tracy")

# ---------------------------------------------------------------------------
# Track D — Assimp-backed FBX/GLB mech importer (vendored).
#
# Default ON. When OFF, the importer .cpp body is gated out and mech3d.cpp
# falls through to the legacy ASE path unconditionally. Stock install must
# remain playable regardless of this flag.
#
# Vendoring rationale: deterministic builds, no network at configure time,
# matches 3rdparty/tracy precedent. Source lives at 3rdparty/assimp/
# (master branch tarball; pin to a release tag later once MVP stabilises).
#
# Spec: docs/superpowers/specs/2026-04-27-assimp-mech-importer-design.md
# Plan: docs/superpowers/plans/2026-04-27-assimp-mech-importer.md
# Findings (advisor review): docs/superpowers/explorations/2026-05-02-track-d-mvp-adversarial-findings.md
# ---------------------------------------------------------------------------
option(ENABLE_ASSIMP_IMPORTER "Enable FBX/GLB mech import via Assimp (Track D)" ON)
option(ENABLE_CDAG_COOKER "Build the cdag_cooker host tool (requires meshoptimizer; never linked into mc2)" OFF)

if(ENABLE_ASSIMP_IMPORTER)
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/assimp/CMakeLists.txt")
        message(FATAL_ERROR
            "ENABLE_ASSIMP_IMPORTER=ON but 3rdparty/assimp/ is empty. "
            "Vendor Assimp source there (master branch from "
            "https://github.com/assimp/assimp), or set -DENABLE_ASSIMP_IMPORTER=OFF.")
    endif()
    # Prune Assimp's build surface to the importers we actually need.
    set(ASSIMP_BUILD_TESTS                    OFF CACHE BOOL "" FORCE)
    set(ASSIMP_BUILD_ASSIMP_TOOLS             OFF CACHE BOOL "" FORCE)
    set(ASSIMP_INSTALL                        OFF CACHE BOOL "" FORCE)
    set(ASSIMP_BUILD_ALL_EXPORTERS_BY_DEFAULT OFF CACHE BOOL "" FORCE)
    set(ASSIMP_BUILD_ALL_IMPORTERS_BY_DEFAULT OFF CACHE BOOL "" FORCE)
    set(ASSIMP_BUILD_FBX_IMPORTER             ON  CACHE BOOL "" FORCE)
    set(ASSIMP_BUILD_GLTF_IMPORTER            ON  CACHE BOOL "" FORCE)
    set(ASSIMP_WARNINGS_AS_ERRORS             OFF CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS                     OFF CACHE BOOL "" FORCE)
    # Assimp's Compression.cpp uses Z_TREES (zlib >= 1.2.4); our system zlib
    # is 1.2.3. Force Assimp to build + use its own bundled zlib.
    # ASSIMP_BUILD_ZLIB alone isn't enough — Assimp's CMakeLists.txt skips its
    # own zlib if ZLIB_FOUND is already true, which it is because the root
    # find_package(ZLIB) ran at line 131. Reset ZLIB_FOUND in this scope so
    # Assimp's contrib/zlib path (line 586 in vendored CMakeLists.txt) wins.
    # The engine's own ZLIB::ZLIB target is unaffected (already created).
    set(ASSIMP_BUILD_ZLIB                     ON  CACHE BOOL "" FORCE)
    set(ZLIB_FOUND FALSE)
    # [ASSIMP-PDB-SLIM-1] Build vendored assimp WITHOUT MSVC debug info. Statically
    # linked assimp (FBX+GLTF+OpenDDL+zlib parsers) contributed ~39 MB to mc2.pdb,
    # yet the importer is runtime-gated OFF by default and assimp is 3rdparty we do
    # not step-debug into. Strip /Zi (debug-info generation) from the RelWithDebInfo
    # compile flags for the assimp subdir ONLY, then restore so mc2/mclib/gameos/etc.
    # keep full symbols. SCOPED — no global CMAKE_MSVC_DEBUG_INFORMATION_FORMAT,
    # /DEBUG, /INCREMENTAL, or /ZI change; assimp's own CMake does not add /Zi in the
    # RelWithDebInfo config (only DEBUG/RELEASE), so /Zi here is purely inherited.
    set(_ASSIMP_SLIM_SAVED_CXX "${CMAKE_CXX_FLAGS_RELWITHDEBINFO}")
    set(_ASSIMP_SLIM_SAVED_C   "${CMAKE_C_FLAGS_RELWITHDEBINFO}")
    string(REGEX REPLACE "[-/]Z[iI7]" "" CMAKE_CXX_FLAGS_RELWITHDEBINFO "${CMAKE_CXX_FLAGS_RELWITHDEBINFO}")
    string(REGEX REPLACE "[-/]Z[iI7]" "" CMAKE_C_FLAGS_RELWITHDEBINFO   "${CMAKE_C_FLAGS_RELWITHDEBINFO}")
    # EXCLUDE_FROM_ALL: don't build assimp unless mclib actually links it.
    add_subdirectory(3rdparty/assimp out/3rdparty/assimp EXCLUDE_FROM_ALL)
    # Restore full debug-info flags for all subsequent (engine-owned) targets.
    set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "${_ASSIMP_SLIM_SAVED_CXX}")
    set(CMAKE_C_FLAGS_RELWITHDEBINFO   "${_ASSIMP_SLIM_SAVED_C}")
    message(STATUS "ENABLE_ASSIMP_IMPORTER=ON — vendored Assimp at 3rdparty/assimp/ (FBX + GLTF, static, debug-info stripped: ASSIMP-PDB-SLIM-1)")
endif()

# ---------------------------------------------------------------------------
# Offline asset-pipeline probe host tools (ASSIMP-IMPORTER-PHASE-0 +).
# Isolated subdir so link deps stay minimal — these tools link ONLY
# assimp::assimp, never mclib / gameos / SDL2 / GLEW. Placed AFTER the assimp
# add_subdirectory so assimp::assimp is visible. A later slice extends the
# subdir with meshopt_lod_probe.
# ---------------------------------------------------------------------------
option(ENABLE_ASSET_CPP_PROBES "Build offline asset probe host tools (assimp_probe, meshopt_lod_probe)" OFF)
if(ENABLE_ASSET_CPP_PROBES)
    if(NOT ENABLE_ASSIMP_IMPORTER)
        message(FATAL_ERROR "ENABLE_ASSET_CPP_PROBES=ON requires ENABLE_ASSIMP_IMPORTER=ON (assimp_probe links assimp::assimp).")
    endif()
    # meshoptimizer (vendored, unwired at runtime) is brought in ONLY for the
    # offline meshopt_lod_probe tool (MESHOPT-LOD-PROBE-1). Demo + gltfpack are
    # already OFF by default in its CMakeLists; force them OFF explicitly so the
    # only target produced is the static `meshoptimizer` lib. EXCLUDE_FROM_ALL:
    # never built unless the probe exe (or a runtime target) links it.
    set(MESHOPT_BUILD_DEMO        OFF CACHE BOOL "" FORCE)
    set(MESHOPT_BUILD_GLTFPACK    OFF CACHE BOOL "" FORCE)
    set(MESHOPT_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    add_subdirectory(3rdparty/meshoptimizer "${CMAKE_BINARY_DIR}/out/3rdparty/meshoptimizer" EXCLUDE_FROM_ALL)
    add_subdirectory(tools/cpp_probes "${CMAKE_BINARY_DIR}/out/tools/cpp_probes")
    message(STATUS "ENABLE_ASSET_CPP_PROBES=ON — building offline host tools in tools/cpp_probes/")
endif()

add_subdirectory("./mclib/" "./out/mclib")
add_subdirectory("./RenderCore" "./out/RenderCore")
add_subdirectory("./RenderWorld" "./out/RenderWorld")
add_subdirectory("./GameAdapters" "./out/GameAdapters")
add_subdirectory("./EditorBridge" "./out/EditorBridge")
add_subdirectory("./mclib/fx_trace" "./out/mclib/fx_trace")
add_subdirectory("./mclib/particles" "./out/mclib/particles")
add_subdirectory("./mclib/mlr" "./out/mclib/mlr")
add_subdirectory("./mclib/gosfx" "./out/mclib/gosfx")
add_subdirectory("./mclib/stuff" "./out/mclib/stuff")
add_subdirectory("./gui" "./out/gui")
add_subdirectory("./GameOS/gameos" "./out/GameOS/gameos")
if(MC2_RNG_TRACE)
    target_compile_definitions(gameos PRIVATE MC2_RNG_TRACE=1)
endif()

add_subdirectory("./GameOS/src" "./out/windows")
add_subdirectory("3rdparty/imgui")
add_subdirectory("GuiRuntime")
# Standalone ImGui UI Editor.
# Guarded so source snapshots that carry only changelog/version files do
# not break configure, while full local UI Editor trees produce ui_editor.vcxproj.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/ui_editor/CMakeLists.txt")
    add_subdirectory("./ui_editor" "./out/ui_editor")
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tools/asset_viewer/CMakeLists.txt")
    add_subdirectory("./tools/asset_viewer" "./out/tools/asset_viewer")
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tools/mc2_launcher/CMakeLists.txt")
    add_subdirectory("./tools/mc2_launcher" "./out/tools/mc2_launcher")
endif()
# mc2fx — standalone gosFX .fx blob inspector (opt-in; curated game-free target).
if(ENABLE_MC2FX AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tools/mc2fx/CMakeLists.txt")
    add_subdirectory("./tools/mc2fx" "./out/tools/mc2fx")
endif()
# mc2fx_preview — GUI curve previewer (SDL2+GL+ImGui; reuses mc2fx_core).
if(ENABLE_MC2FX_PREVIEW AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tools/mc2fx_preview/CMakeLists.txt")
    add_subdirectory("./tools/mc2fx_preview" "./out/tools/mc2fx_preview")
endif()
# mc2weapon_gui — ImGui weapon editor (SDL2+GL+ImGui; CSV-only, no engine).
if(ENABLE_MC2WEAPON_GUI AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tools/mc2weapon_gui/CMakeLists.txt")
    add_subdirectory("./tools/mc2weapon_gui" "./out/tools/mc2weapon_gui")
endif()


set(SOURCES 
    "code/dw.cpp"
    "code/mechcmd2.cpp"
    "code/artlry.cpp"
    "code/bldng.cpp"
    "code/carnage.cpp"
    "code/collsn.cpp"
    "code/comndr.cpp"
    "code/contact.cpp"
    # code/frame_jobs.cpp moved to mclib (LINK-CONFIG-FIX): mclib/msl.cpp
    # references g_isFrameJobsWorker, so the defining TU must live in mclib to
    # resolve EditRel/data_tools. Listing it here too would double-define it.
    "code/gameobj.cpp"
    "code/gate.cpp"
    "code/goal.cpp"
    "code/group.cpp"
    "code/gvehicl.cpp"
    "code/light.cpp"
    "code/mech.cpp"
    "code/mover.cpp"
    "code/objmgr.cpp"
    "code/objtype.cpp"
    "code/unitprofile.cpp"
    "code/unitprofile_fit.cpp"
    "code/equipment_registry_engine.cpp"
    "code/ui_profile_registry.cpp"
    "code/action_executor_registry.cpp"
    "code/tacordr.cpp"
    "code/team.cpp"
    "code/terrobj.cpp"
    "code/turret.cpp"
    "code/brain_special_dispatch.cpp"
    "code/brain_symbol_resolve.cpp"
    "code/brain_missionfit_oporbd.cpp"
    "code/brain_tactic_select.cpp"
    "code/warrior.cpp"
    "code/weaponbolt.cpp"
    "code/ablmc2.cpp"
    "code/gamecam.cpp"
    "code/mission.cpp"
    "code/mission2.cpp"
    "code/objective.cpp"
    "code/saveload.cpp"
    "code/trigger.cpp"
    "code/weather.cpp"
    "gui/aanimobject.cpp"
    "gui/aedit.cpp"
    "code/attributemeter.cpp"
    "code/controlgui.cpp"
    "code/forcegroupbar.cpp"
    "code/gametacmap.cpp"
    "code/infowindow.cpp"
    "code/keyboardref.cpp"
    "code/mechicon.cpp"
    "code/missiongui.cpp"
    "code/tacticaloverview_state.cpp"
    "code/tacticaloverview.cpp"
    "code/gameplay_pick.cpp"
    "code/mpconnectiontype.cpp"
    "code/mpgamebrowser.cpp"
    "code/mphostgame.cpp"
    "code/mploadmap.cpp"
    "code/mpparameterscreen.cpp"
    "code/mpsetuparea.cpp"
    "code/optionsarea.cpp"
    "code/optionsscreenwrapper.cpp"
    "code/pausewindow.cpp"
    "code/prefs.cpp"
    "code/simplecamera.cpp"
    "code/componentlistbox.cpp"
    "code/loadscreen.cpp"
    "code/logistics.cpp"
    "code/logisticscomponent.cpp"
    "code/logisticsdata.cpp"
    "code/logisticsdialog.cpp"
    "code/logisticsmech.cpp"
    "code/logisticsmechdisplay.cpp"
    "code/logisticsmechicon.cpp"
    "code/logisticsmissioninfo.cpp"
    "code/logisticspilot.cpp"
    "code/logisticspilotlistbox.cpp"
    "gui/logisticsscreen.cpp"
    "code/logisticsvariant.cpp"
    "code/mainmenu.cpp"
    "code/mechbayscreen.cpp"
    "code/mechlabscreen.cpp"
    "code/mechlistbox.cpp"
    "code/mechlopedia.cpp"
    "code/mechpurchasescreen.cpp"
    "code/missionbegin.cpp"
    "code/missionbriefingscreen.cpp"
    "code/missionresults.cpp"
    "code/missionselectionscreen.cpp"
    "code/mpprefs.cpp"
    "code/pilotreadyscreen.cpp"
    "code/pilotreviewarea.cpp"
    "code/salvagemecharea.cpp"
    "code/simplecomponentlistbox.cpp"
    "code/gamesound.cpp"
    "code/radio.cpp"
    "code/movemgr.cpp"
    "code/chatwindow.cpp"
    "code/mpstats.cpp"
    "code/multplyr.cpp"
    "code/mc2movie.cpp"
    "code/mc2video.cpp"
    )


include_directories(${COMMON_INCLUDE_DIRS} "./mclib" "./gui" "./code" "./netlib")
include_directories(${THIRDPARTY_INCLUDE_DIRS})

if(NOT WIN32)
	set(ADDITIONAL_LIBS dl)
	message("Not win32 system")
else()
	# winmm for timeGetTime, maybe switch to GetTickCount to remove this dependency
	set(ADDITIONAL_LIBS winmm ws2_32 dbghelp psapi user32 shell32 ole32 windowscodecs uuid)
endif()


# Deploy fingerprint: embed git identity (sha/dirty/branch/build-time/worktree)
# in the exe. Regenerated on every BUILD (not just configure) by the custom
# target below; the script writes only-if-changed so no-op rebuilds stay no-op.
# Consumed by code/logmain.cpp ("[BUILD_FINGERPRINT v1]" startup banner) and
# asserted by scripts/run_smoke.py.
set(MC2_BUILD_FINGERPRINT_H "${CMAKE_BINARY_DIR}/generated/build_fingerprint.h")
execute_process(COMMAND ${CMAKE_COMMAND}
        -DOUT=${MC2_BUILD_FINGERPRINT_H} -DSRC_DIR=${CMAKE_SOURCE_DIR}
        -P "${CMAKE_SOURCE_DIR}/cmake/GenerateBuildFingerprint.cmake")
add_custom_target(build_fingerprint
    COMMAND ${CMAKE_COMMAND}
        -DOUT=${MC2_BUILD_FINGERPRINT_H} -DSRC_DIR=${CMAKE_SOURCE_DIR}
        -P "${CMAKE_SOURCE_DIR}/cmake/GenerateBuildFingerprint.cmake"
    BYPRODUCTS "${MC2_BUILD_FINGERPRINT_H}"
    COMMENT "Refreshing build_fingerprint.h (git sha + dirty flag)"
    VERBATIM)

add_executable(mc2 ${SOURCES} ${MAIN_SRC})
add_dependencies(mc2 build_fingerprint)
target_link_libraries(mc2 mclib fx_trace particles gosfx mlr stuff gui gameos gameos_main windows ZLIB::ZLIB ${SDL2_LIBRARIES} GLEW::GLEW SDL2_mixer::SDL2_mixer ${ADDITIONAL_LIBS} OpenGL::GL renderworld gameadapters editorbridge imgui gui_runtime)

if(MC2_IMGUI)
    target_compile_definitions(mc2 PRIVATE MC2_IMGUI=1)
    target_compile_definitions(gui_runtime PRIVATE MC2_IMGUI=1)
    target_compile_definitions(gameos PRIVATE MC2_IMGUI=1)
    target_compile_definitions(gameos_main PRIVATE MC2_IMGUI=1)
    # gameosmain.cpp includes imgui_impl_sdl2.h when MC2_IMGUI is set
    target_include_directories(gameos_main PRIVATE
        "${CMAKE_SOURCE_DIR}/3rdparty/imgui"
        "${CMAKE_SOURCE_DIR}/3rdparty/imgui/backends"
        "${CMAKE_SOURCE_DIR}/GuiRuntime"
    )
    # imgui + gui_runtime symbols resolve at mc2 link; no target_link_libraries
    # needed here for a static-lib-only target in a subdirectory.
endif()

target_link_libraries(mc2
    FFmpegLGPL::avformat
    FFmpegLGPL::avcodec
    FFmpegLGPL::avutil
    FFmpegLGPL::swscale
    FFmpegLGPL::swresample)

# Delay-load all five FFmpeg DLLs so mc2.exe launches even when the
# DLLs are missing (the game gracefully skips video playback). The
# delay-load failure hook lives in code/mc2video.cpp.
if(MSVC)
    target_link_options(mc2 PRIVATE
        "/DELAYLOAD:${FFmpegLGPL_avcodec_DLL_NAME}"
        "/DELAYLOAD:${FFmpegLGPL_avformat_DLL_NAME}"
        "/DELAYLOAD:${FFmpegLGPL_avutil_DLL_NAME}"
        "/DELAYLOAD:${FFmpegLGPL_swscale_DLL_NAME}"
        "/DELAYLOAD:${FFmpegLGPL_swresample_DLL_NAME}")
    target_link_libraries(mc2 delayimp.lib)
endif()

# FFmpeg runtime DLLs — must sit next to mc2.exe (delay-load pattern
# does not support subdirectories without SetDllDirectory calls).
foreach(_dll ${FFmpegLGPL_RUNTIME_DLLS})
    add_custom_command(TARGET mc2 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_dll}" "$<TARGET_FILE_DIR:mc2>")
endforeach()
install(FILES ${FFmpegLGPL_RUNTIME_DLLS} DESTINATION ".")

# Other runtime DLLs (SDL2/GLEW/ZLIB/mc2res). Same rationale as the
# editor target — without these mc2.exe fails at launch with a
# missing-DLL error after a clean build. Root floor is CMake 3.10, so
# TARGET_RUNTIME_DLLS (3.21+) is unavailable; we use TARGET_FILE for
# imported targets whose IMPORTED_LOCATION is the .dll (SDL2/SDL2_mixer/
# mc2res) and explicit filenames for GLEW/ZLIB (their IMPORTED_LOCATION
# points at the .lib import library). vcpkg colocates all DLLs next to
# SDL2.dll, so anchor on $<TARGET_FILE_DIR:SDL2::SDL2>.
set(_mc2_runtime_dll_targets
    SDL2::SDL2
    SDL2_mixer::SDL2_mixer
    mc2res_64
)
foreach(_t IN LISTS _mc2_runtime_dll_targets)
    add_custom_command(TARGET mc2 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:${_t}>" "$<TARGET_FILE_DIR:mc2>"
        VERBATIM
    )
endforeach()

foreach(_dll_name glew32.dll zlib1.dll)
    add_custom_command(TARGET mc2 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE_DIR:SDL2::SDL2>/${_dll_name}"
                "$<TARGET_FILE_DIR:mc2>"
        VERBATIM
    )
endforeach()

# ASan runtime DLL must sit next to mc2.exe. Without it the exe fails to
# launch with status 0xC0000139 (entry point not found). The DLL ships with
# the MSVC toolset, so we resolve it relative to the selected compiler
# rather than hardcoding a path.
#
# Resolution strategy:
#   1. Arch suffix from CMAKE_SIZEOF_VOID_P (x86_64 / i386). No ARM support
#      — this project is x64-only per the top-level 64-bit check.
#   2. Primary location: same directory as cl.exe (Hostx64/x64 for a 64-bit
#      VS generator). This is where current MSVC puts the ASan runtime.
#   3. Fallback: walk up to the VCToolsInstallDir and try the Redist tree,
#      which is where older installers stage the runtime.
#   4. FATAL_ERROR with the attempted paths if neither exists.
if(MC2_ASAN)
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(_asan_arch "x86_64")
    elseif(CMAKE_SIZEOF_VOID_P EQUAL 4)
        set(_asan_arch "i386")
    else()
        message(FATAL_ERROR "MC2_ASAN: unsupported pointer size ${CMAKE_SIZEOF_VOID_P}")
    endif()
    set(_asan_dll_name "clang_rt.asan_dynamic-${_asan_arch}.dll")

    get_filename_component(_msvc_bindir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set(_asan_candidates
        "${_msvc_bindir}/${_asan_dll_name}"
    )
    # Walk up from .../Tools/MSVC/<ver>/bin/Hostx64/x64 to the MSVC install
    # root and probe the Redist subtree used by older toolsets.
    get_filename_component(_msvc_tools_ver_dir "${_msvc_bindir}/../../.."   ABSOLUTE)
    if(EXISTS "${_msvc_tools_ver_dir}/bin/Hostx64/x64/${_asan_dll_name}")
        list(APPEND _asan_candidates
             "${_msvc_tools_ver_dir}/bin/Hostx64/x64/${_asan_dll_name}")
    endif()

    set(_asan_dll "")
    foreach(_c ${_asan_candidates})
        if(EXISTS "${_c}")
            set(_asan_dll "${_c}")
            break()
        endif()
    endforeach()
    if(NOT _asan_dll)
        string(REPLACE ";" "\n  " _asan_tried "${_asan_candidates}")
        message(FATAL_ERROR
            "MC2_ASAN=ON but ${_asan_dll_name} not found. Tried:\n  ${_asan_tried}")
    endif()
    message(STATUS "MC2_ASAN: copying ${_asan_dll} to output dir at POST_BUILD")
    add_custom_command(TARGET mc2 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_asan_dll}" "$<TARGET_FILE_DIR:mc2>")
endif()

add_subdirectory("./res" "./out/res")
add_subdirectory("./data_tools" "./out/data_tools")
add_subdirectory("./text_tool" "./out/text_tool")
# TEMP (merge-methuselas throwaway): @Methuselas's Viewer/CMakeLists.txt lists
# ImGui-conversion sources (ViewerHostGlobals/Imgui/GuiFit/FontCatalog/
# TextureLoader.cpp) that are NOT committed to methuselas/main -- one missing
# source fails the whole CMake generate step. Disabled so ui_editor + mc2 can
# build. Re-enable once those sources land. Not a real fix; do not upstream.
# add_subdirectory("./Viewer" "./out/Viewer")

# MC2 Mission Editor (MFC/Windows-only). editor/CMakeLists.txt guards
# itself with if(NOT MSVC) return() so this is safe on all platforms.
# Scoped import from PR #36: editor tree + gameos_editor only; PR's
# terrain-shader/.fit/resource rework deliberately NOT merged.
add_subdirectory("./editor" "./out/editor")

# -----------------------------------------------------------------------
# Shader contract reflection CI (Tier 1.2)
# Depends on Vulkan SDK tools: glslangValidator, spirv-cross.
# Run: cmake --build build64 --target shader_reflect
# -----------------------------------------------------------------------
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
    add_custom_target(shader_reflect
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tools/shader_reflect/reflect.py
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Shader contract reflection CI (Tier 1.2)"
        VERBATIM
    )
    if(TARGET shader_validate)
        add_dependencies(shader_reflect shader_validate)
    endif()
endif()

# ---------------------------------------------------------------------------
# NS3 boundary-enforcement instrument (design C: drop gameos_main).
# Links engine libs ONLY (not gameos_main, not gui, not any code/) with
# /WHOLEARCHIVE so every engine object is forced in. The unresolved-symbol
# set is the complete mclib/engine -> code boundary (the NS3 metric).
# Default OFF so it never blocks the normal mc2 build.
# Spec: docs/superpowers/specs/2026-05-18-mclib-standalone-boundary-enforcement-spec.md
option(MC2_BUILD_ENGINE_STANDALONE "NS3: link engine libs without code/ (boundary-enforcement instrument)" OFF)
if(MC2_BUILD_ENGINE_STANDALONE)
    add_executable(mclib_standalone tools/engine_standalone_stub.cpp)
    target_link_libraries(mclib_standalone
        mclib gosfx mlr stuff gameos windows
        ZLIB::ZLIB ${SDL2_LIBRARIES} GLEW::GLEW SDL2_mixer::SDL2_mixer ${ADDITIONAL_LIBS} OpenGL::GL)
    if(MSVC)
        target_link_options(mclib_standalone PRIVATE
            /WHOLEARCHIVE:mclib
            /WHOLEARCHIVE:gosfx
            /WHOLEARCHIVE:mlr
            /WHOLEARCHIVE:stuff
            /WHOLEARCHIVE:gameos
            /WHOLEARCHIVE:windows)
    endif()
endif()

# ---------------------------------------------------------------------------
# env_registry: MC2_* env-var coverage gate
#
# Runs scripts/check-env-registry.sh, which verifies every MC2_* env var
# used in source is either registered in RenderCore/RendererFeatureRegistry.h
# or explicitly allowlisted. Exits nonzero on unknowns.
#
# Run manually: cmake --build build64 --target env_registry
# ---------------------------------------------------------------------------
# Prefer Git-bundled bash over the WSL stub in System32.
find_program(BASH_EXECUTABLE
    NAMES bash
    HINTS "C:/Program Files/Git/bin" "C:/Program Files/Git/usr/bin"
    DOC "Git bash (preferred over WSL stub)"
)
if(BASH_EXECUTABLE)
    add_custom_target(env_registry
        COMMAND ${BASH_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/check-env-registry.sh
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "MC2_* env-var registry coverage gate (RendererFeatureRegistry.h)"
        VERBATIM
    )
endif()

# ---------------------------------------------------------------------------
# shader_schema: C++/GLSL ABI validation gate (SHADER-SCHEMA-1)
#
# Validates SSBO struct layout contracts against shader_reflect golden JSON.
# Checks struct sizes and field offsets for high-risk C++/GLSL interfaces.
# Requires shader_reflect goldens to exist (run shader_reflect --update first).
#
# Run manually: cmake --build build64 --target shader_schema
# ---------------------------------------------------------------------------
if(Python3_FOUND AND BASH_EXECUTABLE)
    add_custom_target(shader_schema
        COMMAND ${BASH_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/check-shader-schema.sh
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Shader ABI validation — C++/GLSL layout contract gate"
        VERBATIM
    )
endif()

# ---------------------------------------------------------------------------
# MC2_BUILD_TESTS: unit/integration test targets (model_override_tests, etc.)
# ---------------------------------------------------------------------------
option(MC2_BUILD_TESTS "Build unit/integration test targets" ON)
if(MC2_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests/model_override)
  add_subdirectory(tests/static_natural_skip)
endif()
