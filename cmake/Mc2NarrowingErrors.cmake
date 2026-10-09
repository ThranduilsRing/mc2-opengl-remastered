# Warnings-as-errors for implicit narrowing, per target.
#
# WHY: size_t->int/long truncation (C4267), lossy arithmetic conversion (C4244),
# printf/scanf format-vs-argument mismatch (C4477), int->pointer of a smaller
# int (C4312) and signed/unsigned compares (C4018) are real 64-bit bugs. Once a
# target reads zero for them they must stay zero; the codes are promoted to
# errors only for targets that are clean, so noisy ones keep building.
#
# To bring a target in: get it to zero, then add its name to the call below.
# Third-party and not-yet-clean targets are deliberately absent.

option(MC2_NARROWING_ERRORS "Promote narrowing/format/pointer-size warnings to errors in clean targets" ON)

set(MC2_NARROWING_WARNING_CODES 4267 4244 4477 4312 4018)

function(mc2_narrowing_errors)
    if(NOT MC2_NARROWING_ERRORS OR NOT MSVC)
        return()
    endif()
    foreach(_target IN LISTS ARGN)
        if(NOT TARGET ${_target})
            message(FATAL_ERROR "mc2_narrowing_errors: no such target '${_target}'")
        endif()
        foreach(_code IN LISTS MC2_NARROWING_WARNING_CODES)
            target_compile_options(${_target} PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:/we${_code}>")
        endforeach()
    endforeach()
endfunction()

# Per-source variant for targets that are only partly clean. Sources are matched by
# absolute path; ONLY_UNDER limits to given directories, EXCEPT removes files that
# are still noisy (or owned by another in-flight lane). Needs CMake >= 3.18.
function(mc2_narrowing_errors_sources target)
    if(NOT MC2_NARROWING_ERRORS OR NOT MSVC)
        return()
    endif()
    if(CMAKE_VERSION VERSION_LESS 3.18)
        message(STATUS "mc2_narrowing_errors_sources: CMake < 3.18, per-source /we skipped for ${target}")
        return()
    endif()
    cmake_parse_arguments(ARG "" "" "ONLY_UNDER;EXCEPT" ${ARGN})
    get_target_property(_srcs ${target} SOURCES)
    get_target_property(_dir ${target} SOURCE_DIR)
    foreach(_s IN LISTS _srcs)
        if(NOT _s MATCHES "[.]cpp$")
            continue()
        endif()
        get_filename_component(_abs "${_s}" ABSOLUTE BASE_DIR "${_dir}")
        if(ARG_ONLY_UNDER)
            set(_in FALSE)
            foreach(_u IN LISTS ARG_ONLY_UNDER)
                get_filename_component(_uabs "${_u}" ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}")
                string(FIND "${_abs}" "${_uabs}/" _pos)
                if(_pos EQUAL 0)
                    set(_in TRUE)
                endif()
            endforeach()
            if(NOT _in)
                continue()
            endif()
        endif()
        set(_skip FALSE)
        foreach(_x IN LISTS ARG_EXCEPT)
            get_filename_component(_xabs "${_x}" ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}")
            if(_abs STREQUAL _xabs)
                set(_skip TRUE)
            endif()
        endforeach()
        if(_skip)
            continue()
        endif()
        foreach(_code IN LISTS MC2_NARROWING_WARNING_CODES)
            set_property(SOURCE "${_abs}" TARGET_DIRECTORY ${target} APPEND PROPERTY COMPILE_OPTIONS "/we${_code}")
        endforeach()
    endforeach()
endfunction()

mc2_narrowing_errors(
    mlr
    fx_trace
    rendercore
    renderworld
    gameadapters
    editorbridge
    gui
    gui_runtime
    windows
    stuff
    gosfx
    particles
)

# mclib: every file except the ones lane-1 is editing (txmmgr still carries 6 C4267).
# Drop an entry from EXCEPT once the file is clean and no longer in flight.
mc2_narrowing_errors_sources(mclib
    EXCEPT
        mclib/bdactor.cpp
        mclib/mech3d.cpp
        mclib/txmmgr.cpp
        mclib/terrain.cpp)

# gameos: only utils/ is clean; the gameos/*.cpp files are noisy or in flight.
mc2_narrowing_errors_sources(gameos ONLY_UNDER GameOS/gameos/utils)
