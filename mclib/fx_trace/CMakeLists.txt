cmake_minimum_required(VERSION 3.10)
project(fx_trace)

# Neutral fx invocation counter module per integrated plan v6 §1.
# One-way dependency: gosfx/, mlr/, particles/ include fx_trace.h; this lib
# does not depend on any of them. Survives A4 deletion of gosfx/ + mlr/.

set(SOURCES
    fx_trace.cpp
    fx_cost_split.cpp
)

include_directories(${COMMON_INCLUDE_DIRS} "../../" "./")
include_directories(${THIRDPARTY_INCLUDE_DIRS})

add_library(fx_trace ${SOURCES})

# Typed gate registry (Gates/): any source in this library may call gate::get<>(); a library that does
# not link it breaks every standalone tool that links this one (makefst, pak, aseconv, ...).
if(TARGET fx_trace)
    target_link_libraries(fx_trace PUBLIC mc2_gate)
endif()
