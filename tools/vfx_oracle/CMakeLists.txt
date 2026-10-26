# The lifecycle probe needs the real class hierarchy in a separate executable:
# mc2_tests already has independent platform/heap shims and allocation overrides.
file(GLOB VFX_LIFECYCLE_SOURCES
    "${MC2_WORKTREE_ROOT}/mclib/gosfx/*.cpp"
    "${MC2_WORKTREE_ROOT}/mclib/mlr/*.cpp"
    "${MC2_WORKTREE_ROOT}/mclib/particles/*.cpp"
    "${MC2_WORKTREE_ROOT}/mclib/stuff/*.cpp")
add_executable(vfx_lifecycle_tests
    "${MC2_WORKTREE_ROOT}/tools/vfx_oracle/lifecycle_test.cpp"
    "${MC2_WORKTREE_ROOT}/tools/vfx_oracle/lifecycle_platform.cpp"
    ${VFX_LIFECYCLE_SOURCES})
target_compile_features(vfx_lifecycle_tests PRIVATE cxx_std_23)
target_compile_definitions(vfx_lifecycle_tests PRIVATE $<$<PLATFORM_ID:Windows>:PLATFORM_WINDOWS> LINUX_BUILD _CRT_SECURE_NO_WARNINGS)
target_include_directories(vfx_lifecycle_tests PRIVATE
    "${MC2_WORKTREE_ROOT}" "${MC2_WORKTREE_ROOT}/mclib"
    "${MC2_WORKTREE_ROOT}/mclib/stuff" "${MC2_WORKTREE_ROOT}/mclib/gosfx"
    "${MC2_WORKTREE_ROOT}/mclib/mlr" "${MC2_WORKTREE_ROOT}/mclib/particles"
    "${MC2_WORKTREE_ROOT}/GameOS/include" "${MC2_WORKTREE_ROOT}/GameOS/gameos"
    "${MC2_WORKTREE_ROOT}/3rdparty/include" "${MC2_WORKTREE_ROOT}/3rdparty/tracy"
    "${MC2_WORKTREE_ROOT}/tests/unit")
mc2_link_gate(vfx_lifecycle_tests)
mc2_asan_runtime(vfx_lifecycle_tests)
if(WIN32)
    target_link_libraries(vfx_lifecycle_tests PRIVATE Psapi "${MC2_WORKTREE_ROOT}/3rdparty/lib/x64/zlib.lib")
else()
    find_package(ZLIB REQUIRED)
    target_link_libraries(vfx_lifecycle_tests PRIVATE ZLIB::ZLIB)
    target_sources(vfx_lifecycle_tests PRIVATE
        "${MC2_WORKTREE_ROOT}/GameOS/src/platform_winuser.cpp"
        "${MC2_WORKTREE_ROOT}/GameOS/src/platform_stdlib.cpp"
        "${MC2_WORKTREE_ROOT}/GameOS/src/platform_eh.cpp")
endif()
add_dependencies(mc2_tests vfx_lifecycle_tests)
target_sources(mc2_tests PRIVATE "${MC2_WORKTREE_ROOT}/tools/vfx_oracle/lifecycle_driver.cpp")

# Trace formatting has its own diagnostic sink so complete payload bytes remain observable.
add_executable(vfx_combat_trace_tests "${MC2_WORKTREE_ROOT}/tools/vfx_oracle/combat_trace_test.cpp")
target_compile_features(vfx_combat_trace_tests PRIVATE cxx_std_23)
target_include_directories(vfx_combat_trace_tests PRIVATE
    "${MC2_WORKTREE_ROOT}" "${MC2_WORKTREE_ROOT}/3rdparty/include"
    "${MC2_WORKTREE_ROOT}/GameOS/gameos" "${MC2_WORKTREE_ROOT}/tests/unit")
mc2_asan_runtime(vfx_combat_trace_tests)
add_dependencies(mc2_tests vfx_combat_trace_tests)
