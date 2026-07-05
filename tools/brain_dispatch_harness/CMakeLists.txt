cmake_minimum_required(VERSION 3.16)
project(brain_dispatch_harness CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# REPO_ROOT must be passed as -DREPO_ROOT=<abs_path>
if(NOT DEFINED REPO_ROOT)
    message(FATAL_ERROR "REPO_ROOT not set. Pass -DREPO_ROOT=<absolute-path-to-worktree>")
endif()

# ---------------------------------------------------------------------------
# Stub include directory MUST come first so stub headers shadow engine headers.
# Order: stubs/include -> code/ (for brain_special_dispatch.h, mech_brain_runtime.h)
# ---------------------------------------------------------------------------
set(STUB_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/stubs/include")
set(CODE_DIR     "${REPO_ROOT}/code")
set(MCLIB_DIR    "${REPO_ROOT}/mclib")

# brain_special_dispatch.h and mech_brain_runtime.h are LEAF headers — include code/ for them.
# inifile.h stub is in stubs/include/ and shadows the real mclib/inifile.h.

add_executable(brain_dispatch_harness
    harness_main.cpp
    dispatch_wrapper.cpp        # includes stubs THEN code/brain_special_dispatch.cpp
    "${CODE_DIR}/brain_symbol_resolve.cpp"  # UNITQUERY-SETTARGETPRIORITY-1: pure resolver (no engine deps)
    "${CODE_DIR}/fitblockwriter.cpp"        # FITBLOCK-WRITER-1: raw brace-block writer (pure std leaf)
    stubs/objmgr_stub.cpp
    stubs/tacorder_stub.cpp
    stubs/inifile_stub.cpp
    stubs/brain_tick_stub.cpp   # BRAIN-DECISION-INTENT-QUEUE-1: stub for getBrainTickIndex()
    stubs/tac_order_sink.cpp    # BRAIN-DISPATCH-HARNESS-V2: g_activeSink global
)

# Include path order is critical:
#   1. stubs/include   — stub warrior.h, tacordr.h, gameobj.h, objmgr.h, inifile.h, dstd.h
#   2. code/           — brain_special_dispatch.h, mech_brain_runtime.h (LEAF, no engine deps)
# Do NOT include mclib/ at root (pulls inifile.h which drags engine headers)
target_include_directories(brain_dispatch_harness PRIVATE
    "${STUB_INCLUDE}"
    "${CODE_DIR}"
)

# MSVC: disable /permissive- and suppress warnings expected from stubs
if(MSVC)
    target_compile_options(brain_dispatch_harness PRIVATE
        /W3
        /wd4100  # unreferenced formal parameter
        /wd4127  # conditional expression is constant
        /wd4458  # declaration hides class member
        /wd4456  # declaration hides previous local
    )
    # Windows: _putenv_s is in stdlib.h
    target_compile_definitions(brain_dispatch_harness PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

message(STATUS "brain_dispatch_harness: REPO_ROOT=${REPO_ROOT}")
message(STATUS "brain_dispatch_harness: STUB_INCLUDE=${STUB_INCLUDE}")
message(STATUS "brain_dispatch_harness: CODE_DIR=${CODE_DIR}")
