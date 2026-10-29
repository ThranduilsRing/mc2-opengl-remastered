cmake_minimum_required (VERSION 3.10)
project(mc2)

# Opt-in shared compile cache (scripts/configure_ninja.py --compile-cache). sccache cannot cache a compile that writes the shared
# compiler PDB (/Zi + /Fd), so the cached tree swaps /Zi for /Z7 (debug info lives in each .obj; the linker still writes mc2.pdb).
# Every other flag, the sim FP contract included, is untouched. OFF by default: the default tree keeps /Zi.
option(MC2_COMPILE_CACHE "Compile through a shared cache launcher: /Zi becomes /Z7 so the compiles are cacheable" OFF)
if(MC2_COMPILE_CACHE AND MSVC)
    foreach(_mc2_cc_var CMAKE_C_FLAGS_RELWITHDEBINFO CMAKE_CXX_FLAGS_RELWITHDEBINFO CMAKE_C_FLAGS_DEBUG CMAKE_CXX_FLAGS_DEBUG)
        string(REGEX REPLACE "[-/]Z[iI]( |$)" "/Z7\\1" ${_mc2_cc_var} "${${_mc2_cc_var}}")
    endforeach()
    message(STATUS "MC2_COMPILE_CACHE=ON: /Zi -> /Z7 (${CMAKE_CXX_FLAGS_RELWITHDEBINFO})")
endif()

# C++ standard: one cache variable, MC2_CXX_STANDARD (20 or 23; default 23 = MSVC /std:c++23preview).
# The gate registry (Gates/gate.h) needs C++20 class-type template parameters and consteval, so 17 is
# not an option; standalone projects take the same variable through cmake/Mc2Standalone.cmake.
set(MC2_CXX_STANDARD 23 CACHE STRING "C++ standard for all project targets (20|23); default 23 = MSVC /std:c++23preview")
set_property(CACHE MC2_CXX_STANDARD PROPERTY STRINGS 20 23)
if(MC2_CXX_STANDARD LESS 20)
    message(FATAL_ERROR "MC2_CXX_STANDARD=${MC2_CXX_STANDARD}: the gate registry needs C++20 or newer (use 20 or 23)")
endif()
set(CMAKE_CXX_STANDARD ${MC2_CXX_STANDARD})
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# CMake maps C++23 to /std:c++latest on MSVC (C++26 draft + unstable STL); pin the stable preview switch instead.
if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC" AND MC2_CXX_STANDARD EQUAL 23)
    set(CMAKE_CXX23_STANDARD_COMPILE_OPTION "-std:c++23preview")
    set(CMAKE_CXX23_EXTENSION_COMPILE_OPTION "-std:c++23preview")
endif()

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
)
# may have to help find_package bydetting path to FindSDL2.cmake
#set(SDL2_DIR "${CMAKE_PREFIX_PATH}/cmake")

if(NOT MSVC)
    # CXX17-1 2026-05-24: removed dead -std=c++0x (project standard now set via
    # CMAKE_CXX_STANDARD 17 above; this branch is never reached on Windows builds).
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -ggdb")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-unused-local-typedefs -Werror=array-bounds -Wno-switch -Wwrite-strings -Wno-unknown-warning-option -Wunused-variable")
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:-fpermissive>")
    endif()
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
if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} /MTd /MP")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /MP")
    #set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /MP -DWINDOWS_IGNORE_PACKING_MISMATCH")
endif()

# MSBuild always passes /Zc:inline (drops unreferenced inline/COMDAT functions at
# compile time); CMake's Ninja+MSVC command line does not. Without it the Ninja
# dev tree (build64-ninja) emits ~2x larger objects and linked code that differs
# from the VS tree, so both trees must carry the same flag. VS already has it.
if(MSVC AND NOT CMAKE_GENERATOR MATCHES "Visual Studio")
    add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX>:/Zc:inline>")
endif()

# __cplusplus otherwise reads 199711L on MSVC, which misleads feature-testing headers.
if(MSVC)
    add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:/Zc:__cplusplus>")
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
#   * No custom slab remains in mclib/tgl.cpp; GOSVertexPool is plain storage.
#     If a custom slab returns, annotate it with __asan_poison_memory_region.
# ---------------------------------------------------------------------------
option(MC2_ASAN "Build with MSVC AddressSanitizer (parallel dev build)" OFF)
# Precompiled standard-library header (cmake/pch/mc2_std_pch.h) for the three big targets: gameos, mclib, mc2.
option(MC2_PCH "Precompile the standard-library headers for the gameos, mclib and mc2 targets" OFF)
function(mc2_enable_pch target)
    if(MC2_PCH)
        target_precompile_headers(${target} PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:${CMAKE_SOURCE_DIR}/cmake/pch/mc2_std_pch.h>")
    endif()
endfunction()
option(MC2_IMGUI "Enable in-game ImGui UI" OFF)
# MC2_EDITOR: build the MFC/Windows-only Mission Editor (EditRel). The editor is
# ImGui-gated (editor/CMakeLists.txt FATAL_ERRORs if NOT MC2_IMGUI). Default ON so
# every existing build (build64 shipping, build64_vulkan, EditRel) is UNCHANGED.
# Turn OFF for a pure-mc2 build with MC2_IMGUI=OFF (e.g. the Vulkan parity island),
# which otherwise cannot exist because the editor forces MC2_IMGUI=ON.
option(MC2_EDITOR "Build the MC2 Mission Editor (Windows-only, requires MC2_IMGUI=ON)" ON)
option(MC2_EDITOR_IN_ALL "Include EditRel in the default all build (it recompiles gameos a second time)" OFF)
# DETERMINISTIC-RNG-1: compile-in the per-gos_rand() "RNG" JSONL trace. Default OFF
# so stock builds pay nothing; runtime still requires MC2_DETERMINISTIC_RNG=1 +
# MC2_DIAG_TAGS=RNG. Turn ON only for determinism-proof builds.
option(MC2_RNG_TRACE "Compile the gos_rand RNG diagnostic trace (DETERMINISTIC-RNG-1)" OFF)
# The [DT] determinism trace (mclib/sim_trace.h, docs/sim-trace.md): per-frame RNG draw counts, per-draw call stacks, hit/path/target/
# contact/camera events. Compile-time only, no runtime gate; a hunt build turns it ON, a default build has none of it.
# Proof build for the mission-reload leak delta: counts gos_Malloc/gos_Free live blocks and bytes, and reloads the quicksave 8 times
# (mechcmd2_frame.cpp, [ALLOC_RELOAD] lines). Only gameos.cpp, mechcmd2_frame.cpp and artlry.cpp see the define. Never ON in a shipped build.
option(MC2_ALLOC_STATS "Proof build: gos_Malloc live counters and the reload fixture" OFF)
option(MC2_SIM_TRACE "Compile the [DT] determinism trace into mclib and mc2" OFF)
# The ABL tag (one line per standard-routine call, thousands per frame) is its own switch: a hunt that is not about scripts leaves it
# off so the log stays diffable and the game stays fast enough to reach the frame in question.
option(MC2_SIM_TRACE_ABL "With MC2_SIM_TRACE: also trace every ABL standard-routine call" OFF)
# Multiplayer: the network session layer (code/multplyr*.cpp over netlib) is frozen out of the modernization by owner ruling
# (2026-10-11) and compiled out by default. OFF: code/multplyr_null.cpp defines MPlayer as a pointer that stays null and no-op
# bodies for the members the rest of the game names, so the lobby screens still compile and every `if (MPlayer)` path is the
# single-player one; the main menu's multiplayer button is already disabled. ON: the full layer, as before.
option(MC2_MULTIPLAYER "Compile the multiplayer session layer (code/multplyr*.cpp)" OFF)
# Compile-gate the Vulkan backend.
# Default OFF -> zero Vulkan code compiled, zero Vulkan headers/loader required,
# normal build completely unaffected + no Vulkan dependency. When ON, we
# find_package(Vulkan) (the SDK loader/headers), link it into gameos, and
# compile the Vulkan sources under -DMC2_VULKAN.
option(MC2_VULKAN "Compile the Vulkan backend (2026-10-05: default ON; Vulkan is the target backend)" ON)
# G2: Vulkan is the only supported renderer backend. An explicit -DMC2_VULKAN=OFF is rejected here
# instead of silently configuring a tree whose renderer paths are all compiled out.
if(NOT MC2_VULKAN)
    message(FATAL_ERROR
        "MC2_VULKAN=OFF is not supported: Vulkan is the required renderer backend. "
        "Configure with -DMC2_VULKAN=ON (the default).")
endif()
if(MC2_VULKAN)
    find_package(Vulkan REQUIRED)
    message(STATUS "MC2_VULKAN=ON -- Vulkan found: ${Vulkan_LIBRARY} (include ${Vulkan_INCLUDE_DIR})")
endif()

# MC2_VULKAN_ISLAND: compile Vulkan-owned render-region implementations into
# gameos. Default OFF -> byte-identical non-island build. Requires MC2_VULKAN=ON
# (volk + VMA + headers); each runtime path remains separately gated/fail-soft.
option(MC2_VULKAN_ISLAND "Compile Vulkan-owned render regions into gameos (default ON)" ON)
if(MC2_VULKAN_ISLAND AND NOT MC2_VULKAN)
    message(FATAL_ERROR "MC2_VULKAN_ISLAND=ON requires MC2_VULKAN=ON (volk/VMA/Vulkan headers).")
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
    if(NOT WIN32)
        # Linux guests may hide invariant TSC; Tracy uses CLOCK_MONOTONIC_RAW.
        add_compile_definitions(TRACY_TIMER_FALLBACK)
    endif()
endif()

find_package(SDL2 REQUIRED CONFIG REQUIRED COMPONENTS SDL2)
if(WIN32)
    find_package(SDL2 REQUIRED CONFIG REQUIRED COMPONENTS SDL2main)
endif()
find_package(SDL2_mixer REQUIRED)
#required by text_tool
find_package(SDL2_ttf REQUIRED)
find_package(ZLIB REQUIRED)

list(APPEND CMAKE_MODULE_PATH "${MC2_SOURCE_DIR_CMAKE}/cmake")
find_package(FFmpegLGPL REQUIRED)
# Exposes MC2_VIDEO_DLL_LIST (C string array) in ${CMAKE_BINARY_DIR}/generated/mc2video_dlls.h
include_directories("${CMAKE_BINARY_DIR}/generated")

message("Found SDL2 package ${SDL2_LIBRARIES}" )
message("SDL2 prefix: ${SDL2_PREFIX}")
message("Zlib library found in ${ZLIB}")



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
if(NOT WIN32)
    list(APPEND THIRDPARTY_INCLUDE_DIRS "${MC2_SOURCE_DIR_CMAKE}/3rdparty/glm")
endif()
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

# Source/execution charset UTF-8 for first-party code only: set after the vendored subdirectories
# above, since assimp passes its own /source-charset (the two switches are mutually exclusive).
if(MSVC)
    add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:/utf-8>")
endif()

# STL hardening: out-of-range operator[] on vector/span/string/array and similar become a fast-fail
# trap instead of silent memory corruption. Optimized config only (the dev tree builds no Debug). Vendored
# subdirectories above are unaffected, and imgui is bracketed out where it is added.
if(MSVC)
    add_compile_definitions("$<$<CONFIG:RelWithDebInfo>:_MSVC_STL_HARDENING=1>")
endif()

# Sim-math numerical contract (/fp:precise, no FMA contraction) on every first-party TU from here on.
include(cmake/Mc2SimFpContract.cmake)

add_subdirectory("./Gates" "./out/Gates")
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
add_subdirectory("./tools/terrain_cook" "./out/tools/terrain_cook")
if(MC2_RNG_TRACE)
    target_compile_definitions(gameos PRIVATE MC2_RNG_TRACE=1)
endif()
if(MC2_SIM_TRACE)
    target_compile_definitions(mclib PRIVATE MC2_SIM_TRACE=1)
    if(MC2_SIM_TRACE_ABL)
        target_compile_definitions(mclib PRIVATE MC2_SIM_TRACE_ABL=1)
    endif()
endif()

add_subdirectory("./GameOS/src" "./out/windows")
if(MSVC)
    get_directory_property(_defs COMPILE_DEFINITIONS)
    list(REMOVE_ITEM _defs "$<$<CONFIG:RelWithDebInfo>:_MSVC_STL_HARDENING=1>")
    set_directory_properties(PROPERTIES COMPILE_DEFINITIONS "${_defs}")
endif()
add_subdirectory("3rdparty/imgui")
if(MSVC)
    add_compile_definitions("$<$<CONFIG:RelWithDebInfo>:_MSVC_STL_HARDENING=1>")
endif()
add_subdirectory("GuiRuntime")
include(cmake/Mc2NarrowingErrors.cmake)  # per-target /we for narrowing codes; after all lib targets exist
# Standalone ImGui UI Editor: configured always, built only on request (`ninja ui_editor`).
add_subdirectory("./ui_editor" "./out/ui_editor" EXCLUDE_FROM_ALL)
if(WIN32 AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tools/asset_viewer/CMakeLists.txt")
    add_subdirectory("./tools/asset_viewer" "./out/tools/asset_viewer")
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
    "code/mechcmd2_cmdline.cpp"
    "code/mechcmd2_debugwins.cpp"
    "code/mechcmd2_frame.cpp"
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
    "code/ground_contact_blob.cpp"
    "code/group.cpp"
    "code/group_goals.cpp"
    "code/group_orders.cpp"
    "code/gvehicl.cpp"
    "code/gvehicl_control.cpp"
    "code/gvehicl_move.cpp"
    "code/gvehicl_update.cpp"
    "code/gvehicl_weapons.cpp"
    "code/light.cpp"
    "code/mech.cpp"
    "code/mech_damage.cpp"
    "code/mech_init.cpp"
    "code/mech_loadout.cpp"
    "code/mech_move.cpp"
    "code/mech_move_net.cpp"
    "code/mech_type.cpp"
    "code/mech_update.cpp"
    "code/mech_weapons.cpp"
    "code/mover.cpp"
    "code/mover_attack.cpp"
    "code/mover_chunks.cpp"
    "code/mover_combat.cpp"
    "code/mover_diag.cpp"
    "code/mover_init.cpp"
    "code/mover_misc.cpp"
    "code/mover_path.cpp"
    "code/objmgr.cpp"
    "code/objmgr_io.cpp"
    "code/objmgr_update.cpp"
    "code/render_view.cpp"
    "code/objmgr_pick.cpp"
    "code/objmgr_misc.cpp"
    "code/objtype.cpp"
    "code/unitprofile.cpp"
    "code/unitprofile_fit.cpp"
    "code/equipment_registry_engine.cpp"
    "code/package_chassis_registry.cpp"
    "code/logisticsvariant_package.cpp"
    "code/ui_profile_registry.cpp"
    "code/action_executor_registry.cpp"
    "code/fire_executor_registry.cpp"
    "code/weapon_presentation_proof.cpp"
    "code/impact_presentation.cpp"
    "code/tacordr.cpp"
    "code/team.cpp"
    "code/team_los.cpp"
    "code/los_kernel.cpp"
    "code/terrobj.cpp"
    "code/turret.cpp"
    "code/brain_special_dispatch.cpp"
    "code/brain_special_apply.cpp"
    "code/brain_symbol_resolve.cpp"
    "code/brain_missionfit_oporbd.cpp"
    "code/brain_execution_policy.cpp"
    "code/brain_tactic_select.cpp"
    "code/fitblockwriter.cpp"
    "code/warrior.cpp"
    "code/warrior_brain.cpp"
    "code/warrior_combat.cpp"
    "code/save_brain_section.cpp"
    "code/warrior_alarms.cpp"
    "code/warrior_core_orders.cpp"
    "code/warrior_draw.cpp"
    "code/warrior_move.cpp"
    "code/warrior_orders.cpp"
    "code/warrior_queue.cpp"
    "code/warrior_state.cpp"
    "code/warrior_target.cpp"
    "code/weaponbolt.cpp"
    "code/weaponbolt_render.cpp"
    "code/weaponbolt_update.cpp"
    "code/ablmc2.cpp"
    "code/ablmc2_camera.cpp"
    "code/ablmc2_misc.cpp"
    "code/ablmc2_objects.cpp"
    "code/ablmc2_orders.cpp"
    "code/ablmc2_position.cpp"
    "code/ablmc2_queries.cpp"
    "code/ablmc2_roster.cpp"
    "code/ablmc2_repro.cpp"
    "code/ablmc2_state.cpp"
    "code/gamecam.cpp"
    "code/mission.cpp"
    "code/sim_tick_cost.cpp"
    "code/mission_update.cpp"
    "code/mission_update_audit.cpp"
    "code/mission_movers.cpp"
    "code/mission_init.cpp"
    "code/mission_init_load.cpp"
    "code/mission_init_parts.cpp"
    "code/mission_parts_archive.cpp"
    "code/mission2.cpp"
    "code/sim_identity.cpp"
    "code/sim_world_trace.cpp"
    "code/objective.cpp"
    "code/objective_flags.cpp"
    "code/objective_text.cpp"
    "code/saveload.cpp"
    "code/saveload_load.cpp"
    "code/trigger.cpp"
    "code/weather.cpp"
    "gui/aanimobject.cpp"
    "gui/aedit.cpp"
    "code/attributemeter.cpp"
    "code/controlgui.cpp"
    "code/controlgui_hudtabs.cpp"
    "code/controlgui_render.cpp"
    "code/controlgui_objectives.cpp"
    "code/controlgui_update.cpp"
    "code/controlgui_vehicle.cpp"
    "code/controlgui_init.cpp"
    "code/controlgui_chat.cpp"
    "code/forcegroupbar.cpp"
    "code/gametacmap.cpp"
    "code/infowindow.cpp"
    "code/keyboardref.cpp"
    "code/mechicon.cpp"
    "code/mechicon_package_hud.cpp"
    "code/icon_registry.cpp"
    "code/missiongui.cpp"
    "code/missiongui_diag.cpp"
    "code/missiongui_update.cpp"
    "code/missiongui_style.cpp"
    "code/missiongui_orders.cpp"
    "code/missiongui_cursor.cpp"
    "code/missiongui_support.cpp"
    "code/missiongui_debugkeys.cpp"
    "code/missiongui_input.cpp"
    "code/missiongui_uicmd.cpp"
    "code/missiongui_uicmd_fixture.cpp"
    "code/missiongui_uicmd_probe.cpp"
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
    "code/hudimguipanel.cpp"
    "code/hudtabregistry.cpp"
    "code/hud_imgui_font.cpp"
    "code/prefs.cpp"
    "code/prefs_io.cpp"
    "code/simplecamera.cpp"
    "code/componentlistbox.cpp"
    "code/loadscreen.cpp"
    "code/logistics.cpp"
    "code/logistics_begin.cpp"
    "code/logisticscomponent.cpp"
    "code/logisticsdata.cpp"
    "code/logisticsdata_init.cpp"
    "code/logisticsdata_inventory.cpp"
    "code/logisticsdata_save.cpp"
    "code/logisticsdata_availability.cpp"
    "code/logisticsdata_modify.cpp"
    "code/logisticsdialog.cpp"
    "code/logisticsmech.cpp"
    "code/logisticsmechdisplay.cpp"
    "code/logisticsmechicon.cpp"
    "code/logisticsmissioninfo.cpp"
    "code/logisticsmissioninfo_progress.cpp"
    "code/logisticsmissioninfo_query.cpp"
    "code/logisticspilot.cpp"
    "code/logisticspilotlistbox.cpp"
    "gui/logisticsscreen.cpp"
    "code/logisticsvariant.cpp"
    "code/logisticsvariant_stats.cpp"
    "code/mainmenu.cpp"
    "code/mechbayscreen.cpp"
    "code/mechbayscreen_update.cpp"
    "code/mechbayscreen_mechs.cpp"
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
    "code/mc2movie.cpp"
    "code/mc2video.cpp"
    )
if(MC2_MULTIPLAYER)
    list(APPEND SOURCES "code/multplyr.cpp" "code/multplyr_send.cpp" "code/multplyr_handle.cpp")
else()
    list(APPEND SOURCES "code/multplyr_null.cpp")
endif()


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
# Only mc2_build_identity compiles the changing values. Engine sources include
# stable declarations; deploy tooling still reads the metadata header.
set(MC2_BUILD_FINGERPRINT_H "${CMAKE_BINARY_DIR}/generated/build_fingerprint.h")
set(MC2_BUILD_FINGERPRINT_CPP "${CMAKE_BINARY_DIR}/generated/build_fingerprint.cpp")
execute_process(COMMAND ${CMAKE_COMMAND}
        -DOUT=${MC2_BUILD_FINGERPRINT_H} -DCPP_OUT=${MC2_BUILD_FINGERPRINT_CPP} -DSRC_DIR=${CMAKE_SOURCE_DIR}
        -P "${CMAKE_SOURCE_DIR}/cmake/GenerateBuildFingerprint.cmake")
add_custom_target(build_fingerprint
    COMMAND ${CMAKE_COMMAND}
        -DOUT=${MC2_BUILD_FINGERPRINT_H} -DCPP_OUT=${MC2_BUILD_FINGERPRINT_CPP} -DSRC_DIR=${CMAKE_SOURCE_DIR}
        -P "${CMAKE_SOURCE_DIR}/cmake/GenerateBuildFingerprint.cmake"
    BYPRODUCTS "${MC2_BUILD_FINGERPRINT_H}" "${MC2_BUILD_FINGERPRINT_CPP}"
    COMMENT "Refreshing build fingerprint (one translation unit)"
    VERBATIM)

add_library(mc2_build_identity STATIC "${MC2_BUILD_FINGERPRINT_CPP}")
target_include_directories(mc2_build_identity PRIVATE "${CMAKE_SOURCE_DIR}/GameOS/include")
add_dependencies(mc2_build_identity build_fingerprint)


add_executable(mc2 ${SOURCES} ${MAIN_SRC})
if(MC2_ALLOC_STATS)
    set_source_files_properties("code/mechcmd2_frame.cpp" "code/artlry.cpp" PROPERTIES COMPILE_DEFINITIONS MC2_ALLOC_STATS=1)
    set_source_files_properties("${CMAKE_SOURCE_DIR}/GameOS/gameos/gameos.cpp" DIRECTORY "${CMAKE_SOURCE_DIR}/GameOS/gameos" PROPERTIES COMPILE_DEFINITIONS MC2_ALLOC_STATS=1)
endif()
if(MC2_SIM_TRACE)
    target_compile_definitions(mc2 PRIVATE MC2_SIM_TRACE=1)
endif()
if(MC2_MULTIPLAYER)
    # Only the one TU that creates the session layer reads the define, so toggling the option recompiles four files, not mc2.
    set_source_files_properties("code/logisticsdata.cpp" PROPERTIES COMPILE_DEFINITIONS MC2_MULTIPLAYER=1)
endif()
mc2_enable_pch(mc2)
# Sources whose code the precompiled header would change are compiled without it (generated by tools/pch_audit.py). mclib and gameos
# are defined in subdirectories, so the property is set with their directory scope; this runs after both exist.
if(MC2_PCH)
    include("${CMAKE_SOURCE_DIR}/cmake/pch/pch_skip.cmake")
    foreach(_pch_target gameos mclib mc2)
        set(_pch_skip_abs "")
        foreach(_f ${MC2_PCH_SKIP_${_pch_target}})
            list(APPEND _pch_skip_abs "${CMAKE_SOURCE_DIR}/${_f}")
        endforeach()
        if(_pch_skip_abs)
            set_property(SOURCE ${_pch_skip_abs} TARGET_DIRECTORY ${_pch_target} PROPERTY SKIP_PRECOMPILE_HEADERS ON)
        endif()
    endforeach()
endif()
add_dependencies(mc2 build_fingerprint)
# VULKAN-EDGE-FOG-ISLAND-2a: ensure the SPIR-V shaders (edge_fog.{vert,frag}.spv,
# etc.) are baked when building the mc2 target directly (mc2_vulkan_shaders
# is an ALL target, but --target mc2 does not pull ALL). Only exists under MC2_VULKAN.
if(MC2_VULKAN_ISLAND AND TARGET mc2_vulkan_shaders)
    add_dependencies(mc2 mc2_vulkan_shaders)
endif()
# VULKAN-EDGE-FOG-ISLAND-2b: gos_postprocess.h changes the gosPostProcess class
# LAYOUT under #ifdef MC2_VULKAN_ISLAND (adds vkFogIsland_ + method decls). The
# macro is PUBLIC on gameos, but gameos_main / mc2 do NOT consume gameos's usage
# requirements (gameos_main has no target_link_libraries(gameos)), so define it
# directly on the TUs that include the header, or getSceneFBO()/getWidth()/
# getHeight() read the wrong offset (ODR) -> garbage FBO -> visual capture never
# fires. Harmless when the header block declares only an opaque pointer + decls.
#
# VULKAN-ISLAND-ODR-HARDEN-1: gos_postprocess.h is ALSO included by TUs in mclib,
# renderworld, gameadapters and gui_runtime (all of which link into mc2). Those
# targets do NOT consume gameos's PUBLIC usage requirement (they don't
# target_link_libraries(gameos)), so without the define here they would see the
# smaller (member-absent) gosPostProcess layout -> shifted offsets for any member
# declared after vkFogIsland_ -> ODR mismatch in the ON build. Propagate the
# define to EVERY consuming target so the layout is uniform across the whole link.
# Only added when the option is ON, so build64 (option OFF) stays byte-identical.
if(MC2_VULKAN_ISLAND)
    target_compile_definitions(gameos_main PRIVATE MC2_VULKAN_ISLAND)
    target_compile_definitions(mc2 PRIVATE MC2_VULKAN_ISLAND)
    target_compile_definitions(mclib PRIVATE MC2_VULKAN_ISLAND)
    target_compile_definitions(renderworld PRIVATE MC2_VULKAN_ISLAND)
    target_compile_definitions(gameadapters PRIVATE MC2_VULKAN_ISLAND)
    target_compile_definitions(gui_runtime PRIVATE MC2_VULKAN_ISLAND)
endif()

# VK-P13-SELECTOR-1 / T2: gameosmain.cpp (in the gameos_main target) includes
# vulkan_context.h under #ifdef MC2_VULKAN to birth the shared engine VK device
# at engine init. gameos_main gets MC2_VULKAN + the volk/Vulkan-headers include
# path so that #ifdef is active and <volk.h> resolves. Guarded on MC2_VULKAN so
# the GL-only build (option OFF) is byte-identical (the block never runs -> the
# =vulkan selector fail-softs to GL via the #else path in gameosmain.cpp).
if(MC2_VULKAN)
    target_compile_definitions(gameos_main PRIVATE MC2_VULKAN)
    target_compile_definitions(gui_runtime PRIVATE MC2_VULKAN)
    target_include_directories(gameos_main PRIVATE
        "${CMAKE_SOURCE_DIR}/3rdparty/volk"
        ${Vulkan_INCLUDE_DIR})
    target_include_directories(gui_runtime PRIVATE
        "${CMAKE_SOURCE_DIR}/3rdparty/volk"
        ${Vulkan_INCLUDE_DIR})
endif()
target_link_libraries(mc2 mclib fx_trace particles gosfx mlr stuff gui gameos gameos_main windows ZLIB::ZLIB ${SDL2_LIBRARIES} SDL2_mixer::SDL2_mixer ${ADDITIONAL_LIBS} renderworld gameadapters editorbridge imgui gui_runtime mc2_gate)

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

# Other runtime DLLs (SDL2/ZLIB/mc2res). Same rationale as the
# editor target — without these mc2.exe fails at launch with a
# missing-DLL error after a clean build. Root floor is CMake 3.10, so
# TARGET_RUNTIME_DLLS (3.21+) is unavailable; we use TARGET_FILE for
# imported targets whose IMPORTED_LOCATION is the .dll (SDL2/SDL2_mixer/
# mc2res) and explicit filenames for ZLIB (their IMPORTED_LOCATION
# points at the .lib import library). vcpkg colocates all DLLs next to
# SDL2.dll, so anchor on $<TARGET_FILE_DIR:SDL2::SDL2>.
set(_mc2_runtime_dll_targets "")
if(WIN32)
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

    foreach(_dll_name zlib1.dll)
        add_custom_command(TARGET mc2 POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "$<TARGET_FILE_DIR:SDL2::SDL2>/${_dll_name}"
                    "$<TARGET_FILE_DIR:mc2>"
            VERBATIM
        )
    endforeach()
endif()

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
# The shipped string table (res/libmain.cpp includes test_scripts/res_conv/strings.res.cpp) holds Windows-1252
# bytes inside string literals; reading it as UTF-8 would corrupt them, so mc2res_64 keeps the ANSI source charset.
if(MSVC AND TARGET mc2res_64)
    get_target_property(_res_opts mc2res_64 COMPILE_OPTIONS)
    list(REMOVE_ITEM _res_opts "$<$<COMPILE_LANGUAGE:CXX>:/utf-8>")
    set_property(TARGET mc2res_64 PROPERTY COMPILE_OPTIONS "${_res_opts}")
endif()
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
# MC2_EDITOR gate: editor is ImGui-coupled and forces MC2_IMGUI=ON. Skipping it
# lets mc2 build MC2_IMGUI=OFF (Vulkan parity island). Default ON = no change.
if(MC2_EDITOR AND MSVC)
    add_subdirectory("./editor" "./out/editor")
    # gameos_editor recompiles every gameos source with a different define set (MC2_IMGUI/MC2_RNG_TRACE are
    # only set on gameos), so the objects cannot be shared. Build the editor on request instead of doubling
    # the default build; `ninja EditRel` still builds it. MC2_EDITOR_IN_ALL=ON restores the old default.
    if(NOT MC2_EDITOR_IN_ALL)
        set_target_properties(EditRel gameos_editor PROPERTIES EXCLUDE_FROM_ALL TRUE)
    endif()
endif()

# A class object (std::string, EString) passed through "..." compiles and prints garbage; C4840 is the diagnostic, kept an error so that a printf-style
# call fed a string object fails the build (step 4 batch B1, .claude/cleanup/cxx23-step4-b1-notes.md hazard H10). Applied to the first-party targets that
# hold printf-style logging callers (the foreach below); the other first-party targets are not covered yet.
if(MSVC)
    foreach(_t mc2 mclib gui EditRel gameos gameos_editor gameos_main)
        if(TARGET ${_t})
            target_compile_options(${_t} PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:/we4840>")
        endif()
    endforeach()
endif()

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

target_sources(mc2 PRIVATE code/command_replay.cpp
    code/combat_sim_hash.cpp code/sim_rng.cpp code/tactical_order_queue.cpp code/save_archive.cpp code/save_sections.cpp)
# The sim API must not inherit wall-clock access from the engine PCH.
set_source_files_properties(
    "code/sim_rng.cpp"
    "code/tactical_order_queue.cpp"
    PROPERTIES SKIP_PRECOMPILE_HEADERS ON
)

# Separate engine linkage avoids the standalone tests' initializer/actor stubs.
if(MC2_BUILD_TESTS)
    # The seam links the object files of the mc2 target (same sources, same flags, same definitions) instead of compiling every
    # mc2 source a second time: building it after mc2 costs the test TUs plus one link. A source that mc2 stops compiling still
    # leaves the seam, because the seam IS mc2's object set.
    add_executable(mc2_authored_order_seam EXCLUDE_FROM_ALL
        $<TARGET_OBJECTS:mc2> tests/unit/test_authored_order_seam.cpp
        tests/unit/test_authored_movement_fixture.cpp tests/unit/test_globalmap_init_refusal.cpp
        tests/unit/test_combat_retirement_seam.cpp tests/unit/test_movepath_save_padding.cpp
        tests/unit/test_brain_save_policy_seam.cpp
        tests/unit/test_objtype_init_failure.cpp)
    target_compile_definitions(mc2_authored_order_seam PRIVATE $<TARGET_PROPERTY:mc2,COMPILE_DEFINITIONS>)
    target_include_directories(mc2_authored_order_seam PRIVATE
        $<TARGET_PROPERTY:mc2,INCLUDE_DIRECTORIES> "${CMAKE_SOURCE_DIR}/tests/unit")
    target_link_libraries(mc2_authored_order_seam PRIVATE $<TARGET_PROPERTY:mc2,LINK_LIBRARIES>)
    target_link_options(mc2_authored_order_seam PRIVATE $<TARGET_PROPERTY:mc2,LINK_OPTIONS>)
    add_dependencies(mc2_authored_order_seam build_fingerprint)
    foreach(_dll ${FFmpegLGPL_RUNTIME_DLLS})
        add_custom_command(TARGET mc2_authored_order_seam POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_dll}" "$<TARGET_FILE_DIR:mc2_authored_order_seam>")
    endforeach()
    foreach(_t IN LISTS _mc2_runtime_dll_targets)
        add_custom_command(TARGET mc2_authored_order_seam POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:${_t}>" "$<TARGET_FILE_DIR:mc2_authored_order_seam>")
    endforeach()
    if(WIN32)
        foreach(_dll zlib1.dll)
            add_custom_command(TARGET mc2_authored_order_seam POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE_DIR:SDL2::SDL2>/${_dll}" "$<TARGET_FILE_DIR:mc2_authored_order_seam>")
        endforeach()
    endif()
endif()
