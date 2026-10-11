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
set(CMAKE_CXX_STANDARD ${MC2_CXX_STANDARD})
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# CMake maps C++23 to /std:c++latest on MSVC; pin the stable preview switch like the root project.
if(MSVC AND MC2_CXX_STANDARD EQUAL 23)
    set(CMAKE_CXX23_STANDARD_COMPILE_OPTION "-std:c++23preview")
    set(CMAKE_CXX23_EXTENSION_COMPILE_OPTION "-std:c++23preview")
endif()

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
