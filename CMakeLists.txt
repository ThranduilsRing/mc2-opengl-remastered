cmake_minimum_required (VERSION 3.10)
# select GLVND versus legacy OpenGL libraries
cmake_policy(SET CMP0072 NEW) # cmake --help-policy CMP0072
project(mc2)

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

list(APPEND CMAKE_MODULE_PATH ${CMAKE_PREFIX_PATH}/cmake)
list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake/sdl2)
# may have to help find_package bydetting path to FindSDL2.cmake
#set(SDL2_DIR "${CMAKE_PREFIX_PATH}/cmake")

if(NOT MSVC)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++0x -ggdb")
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

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
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
set(THIRDPARTY_INCLUDE_DIRS "${CMAKE_PREFIX_PATH}/include")
# Tracy headers resolve as <tracy/Tracy.hpp>
list(APPEND THIRDPARTY_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/tracy")

add_subdirectory("./mclib/" "./out/mclib")
add_subdirectory("./mclib/mlr" "./out/mclib/mlr")
add_subdirectory("./mclib/gosfx" "./out/mclib/gosfx")
add_subdirectory("./mclib/stuff" "./out/mclib/stuff")
add_subdirectory("./gui" "./out/gui")
add_subdirectory("./GameOS/gameos" "./out/GameOS/gameos")

add_subdirectory("./GameOS/src" "./out/windows")

set(SOURCES 
    "code/dw.cpp"
    "code/mechcmd2.cpp"
    "code/artlry.cpp"
    "code/bldng.cpp"
    "code/carnage.cpp"
    "code/collsn.cpp"
    "code/comndr.cpp"
    "code/contact.cpp"
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
    "code/tacordr.cpp"
    "code/team.cpp"
    "code/terrobj.cpp"
    "code/turret.cpp"
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
	set(ADDITIONAL_LIBS winmm ws2_32 dbghelp psapi user32 shell32)
endif()


add_executable(mc2 ${SOURCES} ${MAIN_SRC})
target_link_libraries(mc2 mclib gosfx mlr stuff gui gameos gameos_main windows ZLIB::ZLIB ${SDL2_LIBRARIES} GLEW::GLEW SDL2_mixer::SDL2_mixer ${ADDITIONAL_LIBS} OpenGL::GL)

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
add_subdirectory("./Viewer" "./out/Viewer")
# MC2R Editor Remaster
# by Methuselas
# Keep Editor wired as its own optional source tree.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/Editor/CMakeLists.txt")
    add_subdirectory(Editor)
endif()
