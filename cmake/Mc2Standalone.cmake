# cmake/Mc2Standalone.cmake
#
# The language standard for every CMake project that owns a project() and compiles engine
# sources (the standalone harnesses, tools/tg_import_dump, tests/unit, tools/gate_dump).
# The root project sets the same variable in CMakeLists.txt; both honour -DMC2_CXX_STANDARD.
#
# WHY: the typed gate registry (Gates/gate.h) uses class-type template parameters and
# consteval, which need C++20. A harness that still compiled at C++17 would stop building the
# day an engine file it links reads a gate. scripts/check-cxx-standard-floor.py keeps this
# list honest: a project that lists a source reading MC2_* gates must reach C++20.
#
# Usage (before any target):
#   include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/Mc2CxxStandard.cmake")
if(NOT DEFINED MC2_CXX_STANDARD)
    set(MC2_CXX_STANDARD 23 CACHE STRING "C++ standard for all project targets (20|23); 23 = MSVC /std:c++23preview")
    set_property(CACHE MC2_CXX_STANDARD PROPERTY STRINGS 20 23)
endif()
if(MC2_CXX_STANDARD LESS 20)
    message(FATAL_ERROR "MC2_CXX_STANDARD=${MC2_CXX_STANDARD}: the gate registry needs C++20 or newer")
endif()
# Shared compile cache (tools/build_all_targets.py, tools/build_contract_harnesses.py --compile-cache): same /Zi -> /Z7 swap as the root
# project, so a cache launcher can cache the compiles. OFF by default.
option(MC2_COMPILE_CACHE "Compile through a shared cache launcher: /Zi becomes /Z7 so the compiles are cacheable" OFF)
if(MC2_COMPILE_CACHE AND MSVC)
    foreach(_mc2_cc_var CMAKE_C_FLAGS_RELWITHDEBINFO CMAKE_CXX_FLAGS_RELWITHDEBINFO CMAKE_C_FLAGS_DEBUG CMAKE_CXX_FLAGS_DEBUG)
        string(REGEX REPLACE "[-/]Z[iI]( |$)" "/Z7\\1" ${_mc2_cc_var} "${${_mc2_cc_var}}")
    endforeach()
endif()
set(CMAKE_CXX_STANDARD ${MC2_CXX_STANDARD})
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# CMake maps C++23 to /std:c++latest on MSVC; pin the stable preview switch like the root project.
if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC" AND MC2_CXX_STANDARD EQUAL 23)
    set(CMAKE_CXX23_STANDARD_COMPILE_OPTION "-std:c++23preview")
    set(CMAKE_CXX23_EXTENSION_COMPILE_OPTION "-std:c++23preview")
endif()

# The sim-math numerical contract (/fp:precise, no FMA contraction), same as the root project.
include("${CMAKE_CURRENT_LIST_DIR}/Mc2SimFpContract.cmake")

# MC2_ASAN: build the standalone project under MSVC AddressSanitizer (the doctest tier is the one that matters: the ownership step
# runs it natively and under ASan for every batch). Mirrors the root project's option: /fsanitize=address, no /RTC, no incremental
# link; the runtime DLL is copied next to an executable by mc2_asan_runtime(target). ASAN_OPTIONS=detect_leaks=0 is the usual run
# setting (the probe in mclib/alloc_probe.h is the leak oracle, ASan the bounds and lifetime oracle).
option(MC2_ASAN "Build with MSVC AddressSanitizer" OFF)
if(MC2_ASAN)
    if(NOT MSVC)
        message(FATAL_ERROR "MC2_ASAN is only wired for MSVC here; pass -fsanitize=address on other toolchains")
    endif()
    message(STATUS "MC2_ASAN=ON: /fsanitize=address")
    add_compile_definitions(MC2_ASAN=1)
    add_compile_options(/fsanitize=address)
    foreach(_cfg CXX_FLAGS_DEBUG C_FLAGS_DEBUG CXX_FLAGS_RELWITHDEBINFO C_FLAGS_RELWITHDEBINFO)
        string(REGEX REPLACE "/RTC[1csu]+" "" CMAKE_${_cfg} "${CMAKE_${_cfg}}")
    endforeach()
    foreach(_cfg "" "_DEBUG" "_RELWITHDEBINFO" "_RELEASE" "_MINSIZEREL")
        string(REPLACE "/INCREMENTAL" "/INCREMENTAL:NO" CMAKE_EXE_LINKER_FLAGS${_cfg} "${CMAKE_EXE_LINKER_FLAGS${_cfg}}")
    endforeach()
    add_link_options(/INCREMENTAL:NO)
endif()

# Copies clang_rt.asan_dynamic-x86_64.dll next to `target` after it links, when MC2_ASAN is ON (a no-op otherwise), so the executable
# runs from its build directory without PATH surgery. The DLL ships beside cl.exe in current toolsets.
function(mc2_asan_runtime target)
    if(NOT MC2_ASAN)
        return()
    endif()
    get_filename_component(_msvc_bindir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set(_dll "${_msvc_bindir}/clang_rt.asan_dynamic-x86_64.dll")
    if(NOT EXISTS "${_dll}")
        message(FATAL_ERROR "MC2_ASAN=ON but ${_dll} is missing")
    endif()
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_dll}" "$<TARGET_FILE_DIR:${target}>"
        COMMENT "MC2_ASAN: ASan runtime beside ${target}")
endfunction()

# mc2fmt.h (GameOS/include, header-only) is what the printf-to-format conversions of mclib/ and code/ sources include; every standalone project that
# compiles an engine source needs it on its include path, not only the ones that already pull in gameos.hpp.
get_filename_component(_mc2_standalone_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
include_directories("${_mc2_standalone_root}/GameOS/include")

# Link the gate registry (Gates/, STATIC lib mc2_gate) into a standalone target. Linking it
# before any source reads a gate proves the LNK2019 class absent: a project that compiles an
# engine file calling gate::get<> already carries the definitions.
function(mc2_link_gate target)
    get_filename_component(_mc2_root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/.." ABSOLUTE)
    if(NOT TARGET mc2_gate)
        add_subdirectory("${_mc2_root}/Gates" "${CMAKE_BINARY_DIR}/mc2_gate")
    endif()
    target_link_libraries(${target} PRIVATE mc2_gate)
endfunction()
