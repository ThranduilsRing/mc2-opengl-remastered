# tools/job_system_harness/CMakeLists.txt
# MC2-JOBS-CORE-1 — offline unit harness for the domain-tagged job system.
#
# STANDALONE build (no main build64/ cache, no vcpkg, no deps):
#   cmake -S tools/job_system_harness -B build64-h-job_system_harness -A x64
#   cmake --build build64-h-job_system_harness --config RelWithDebInfo \
#         --target job_system_harness
#
# Links the REAL production job core (code/job_system.cpp + code/jobs_fatal.cpp)
# — both self-contained (C++17 stdlib + <thread> only, no GL/gameos) — so the
# determinism/adaptive-fallback/reentrancy/fatal behavior under test is exactly
# the shipped code path. RelWithDebInfo by default so the JOBS_FATAL_IF abort
# proof runs in the same /DNDEBUG config the game and smoke gate use (the check
# is NOT an assert, so it survives NDEBUG — that is the point of the proof).

cmake_minimum_required(VERSION 3.16)
project(job_system_harness CXX)

include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/Mc2Standalone.cmake")

get_filename_component(MC2_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

add_executable(job_system_harness
    job_system_harness.cpp
    ${MC2_ROOT}/code/job_system.cpp
    ${MC2_ROOT}/code/jobs_fatal.cpp)
mc2_link_gate(job_system_harness)

target_include_directories(job_system_harness PRIVATE
    ${MC2_ROOT}/tools/contract_harness_common
    ${MC2_ROOT}/code)
target_compile_features(job_system_harness PRIVATE cxx_std_20)

find_package(Threads REQUIRED)
target_link_libraries(job_system_harness PRIVATE Threads::Threads)
