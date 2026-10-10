# Gates/CMakeLists.txt -- the typed gate registry (design: .claude/cleanup/typed-gate-registry-design.md).
#
# STATIC, not OBJECT: an OBJECT library would duplicate its symbols when linked into several
# static libraries that end up in one executable. A target that reads a gate and forgets to
# link mc2_gate fails at link time (LNK2019), which is the intended failure mode.
# Standalone projects (tools/*_harness, tests/unit) get it through mc2_link_gate() in
# cmake/Mc2Standalone.cmake.
add_library(mc2_gate STATIC
    gate.cpp
    gate_table.cpp)
# PUBLIC include = the repo root, so callers write #include "Gates/gate.h".
target_include_directories(mc2_gate PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/.."
                                   PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(mc2_gate PUBLIC cxx_std_20)
if(MSVC)
    target_compile_options(mc2_gate PRIVATE /utf-8)
endif()
