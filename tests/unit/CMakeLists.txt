# tests/unit/CMakeLists.txt
# doctest-based unit tests for pure-logic MC2 code.
# Tier 2 of docs/testing-strategy.md.
#
# STANDALONE BUILD (preferred -- leaves the main build64/ cache untouched):
#   cmake -S tests/unit -B build64-tests -G "Visual Studio 17 2022" -A x64
#   cmake --build build64-tests --config RelWithDebInfo --target mc2_tests
#   build64-tests/RelWithDebInfo/mc2_tests.exe
#
# Pure-logic only -- the test target deliberately avoids pulling in OpenGL,
# SDL, FFmpeg, or file I/O. Anything dragging those in either:
#   (a) gets factored into a header-only / leaf TU first (preferred --
#       most candidates here are already small files that could land that
#       way), or
#   (b) gets a narrowly-scoped target_sources entry that picks ONE .cpp
#       from the engine without its transitive .cpp neighbors.
#
# Vulkan-prep angle: the more units we get into a no-GL-context test
# target, the more code that survives the eventual RHI port unchanged.
# Tests of pure CPU logic outlive every renderer rewrite.

cmake_minimum_required(VERSION 3.16)
project(mc2_tests CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Resolve the worktree root (this CMakeLists lives at <root>/tests/unit/).
get_filename_component(MC2_WORKTREE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

add_executable(mc2_tests
    test_main.cpp           # owns DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
    test_hashing.cpp
    test_projection.cpp     # currently scope-stopped; placeholder skipped case
    test_argb_pack.cpp
    test_screen_pick.cpp
    test_rendercore.cpp     # RENDERCORE-UNIT-TESTS-1: GL-free substrate invariants
    test_frame_arena.cpp    # FRAME-ARENA-1: FrameArena typed L2 allocator invariants
    test_depth_hzb.cpp      # HZB-DEPTH-CONVENTION-TESTS-1: reverse-Z / HZB reduction contract
    test_host_services.cpp  # ENGINE-HOST-SERVICES-0: host-service contract + default impls
    test_tactical_overview.cpp   # TACTICAL-OVERVIEW-1: blend-state pure logic
    test_xform_convention.cpp    # XFORM-CONVENTION-HARNESS-1: clip-space convention lock
    test_capability.cpp     # UNIT-PROFILE-SEAM-1
    test_view_currency.cpp  # VIEW-EPOCH-DEDUPE-1: object MVP currency kernel (offline)
    test_frame_graph.cpp    # FRAME-GRAPH-SKELETON-1: resource-DAG validation (offline)
    test_gpu_buffer_owner.cpp  # GPU-BUFFER-OWNER-SKELETON-1: owner record invariants (offline)
    test_det_rng.cpp        # DETERMINISTIC-RNG-1: LCG kernel determinism (offline)
    test_vulkan_contract.cpp  # VULKAN-CONTRACT-COVERAGE-1: descriptor-class coverage/consistency (offline)
    test_vulkan_layout_chain.cpp  # VULKAN-LAYOUT-CHAIN-VALIDATOR-2: sampled-image layout-chain coverage/producer (offline)
    test_subgraph_contiguity.cpp  # SUBGRAPH-CONTIGUITY-GUARD-1: postprocess sub-pass contiguity rule (offline)
    test_file_inifile.cpp   # MINIMAL-VIABLE-HARNESS-1: real file/inifile/packet stack, game-free
    test_csvfile.cpp        # MINIMAL-VIABLE-HARNESS-2: real csvfile.cpp (Buildings.csv-shaped)
    test_fst_roundtrip.cpp  # MINIMAL-VIABLE-HARNESS-2: FST create/writeFast -> loose-vs-FST ladder
    test_paths_mod.cpp      # MINIMAL-VIABLE-HARNESS-2: paths.cpp globals + mod-index (TryModOpen) resolution
    test_pilot_profile_preserve.cpp  # PILOT-PROFILE-PRESERVE-1: editor Profile save decision kernel (offline)
    test_ui_defs_lockstep.cpp  # UI-DEFS-LOCKSTEP-HARDEN-1: defs/ui package version + zero-element/zero-draw fail-soft (offline)
    test_hud_tab_registry.cpp  # HUD-IMGUI slice C: tab registry order/cycle invariants (offline)

    # MINIMAL-VIABLE-HARNESS-1: malloc-backed heap + GameOS-global shim.
    # Replaces heap.cpp (won't compile standalone -- see
    # docs/testing/fitini-inmem-harness-bootstrap-recon-1.md) and
    # gameos.cpp/gameos_debugging.cpp (Environment / InternalFunctionStop).
    # heap.cpp MUST NOT be added below while this shim is linked (ODR).
    heap_shim.cpp

    # Leaf-TU engine sources (no transitive engine include drag).
    ${MC2_WORKTREE_ROOT}/mclib/fst_hash.cpp
    ${MC2_WORKTREE_ROOT}/mclib/xform_conventions.cpp  # XFORM-CONVENTION-HARNESS-1: GL-free leaf TU
    ${MC2_WORKTREE_ROOT}/RenderCore/RenderResourceRegistry.cpp
    ${MC2_WORKTREE_ROOT}/RenderCore/RenderDebugView.cpp
    ${MC2_WORKTREE_ROOT}/HostServices/DefaultHostServices.cpp  # ENGINE-HOST-SERVICES-0
    ${MC2_WORKTREE_ROOT}/code/tacticaloverview_state.cpp  # TACTICAL-OVERVIEW-1
    ${MC2_WORKTREE_ROOT}/code/unitprofile.cpp   # UNIT-PROFILE-SEAM-1
    ${MC2_WORKTREE_ROOT}/code/hudtabregistry.cpp  # HUD-IMGUI slice C (pure leaf TU)

    # MINIMAL-VIABLE-HARNESS-1: the real file/ini/packet stack. This is the
    # measured dependency frontier for file.cpp + inifile.cpp (every TU below
    # is required at link time; nothing else from mclib/GameOS is):
    ${MC2_WORKTREE_ROOT}/mclib/file.cpp       # File base: disk/FST/in-RAM backends
    ${MC2_WORKTREE_ROOT}/mclib/inifile.cpp    # FitIniFile block/var parser
    ${MC2_WORKTREE_ROOT}/mclib/packet.cpp     # PacketFile (.pak container)
    ${MC2_WORKTREE_ROOT}/mclib/ffile.cpp      # FastFile (FST archive) -- File::open FST branch
    ${MC2_WORKTREE_ROOT}/mclib/fastfile.cpp   # FastFileFind + fastFiles registry
    ${MC2_WORKTREE_ROOT}/mclib/lzdecomp.cpp   # LZDecomp (ffile+packet read path)
    ${MC2_WORKTREE_ROOT}/mclib/lzcomp.cpp     # LZCompress (ffile writeFast path)
    ${MC2_WORKTREE_ROOT}/mclib/err.cpp        # Fatal/Assert -> STOP (inifile error paths)
    ${MC2_WORKTREE_ROOT}/GameOS/src/platform_str.cpp  # S_strlwr/S_stricmp/S_strnicmp

    # MINIMAL-VIABLE-HARNESS-2: csvfile.cpp is a File subclass -> zero new link
    # frontier (same shim, same base class). Completes the data-ingest spine.
    ${MC2_WORKTREE_ROOT}/mclib/csvfile.cpp    # CSVFile (Excel-CSV reader, Buildings.csv)

    # MINIMAL-VIABLE-HARNESS-2: paths.cpp is a pure leaf TU (path string globals
    # only -- no TryModOpen/mod-index; that logic lives in file.cpp, already
    # linked above). Linking it lets tests reference the engine's real path
    # globals (objectPath/artPath/...) instead of stand-ins.
    ${MC2_WORKTREE_ROOT}/mclib/paths.cpp      # path string globals (leaf)
)

target_include_directories(mc2_tests PRIVATE
    ${MC2_WORKTREE_ROOT}           # for "RenderCore/RenderPassContract.h" style includes
    ${MC2_WORKTREE_ROOT}/mclib
    ${MC2_WORKTREE_ROOT}/GameOS
    ${MC2_WORKTREE_ROOT}/GameOS/include   # XFORM-CONVENTION-HARNESS-1: platform_str.h / gameos.hpp for stuff.hpp chain
    ${MC2_WORKTREE_ROOT}/GameOS/gameos    # XFORM-CONVENTION-HARNESS-1: utils/vec.h pulled by gameos.hpp
    ${MC2_WORKTREE_ROOT}/RenderWorld
    ${MC2_WORKTREE_ROOT}/RenderCore
    ${MC2_WORKTREE_ROOT}/HostServices
    ${MC2_WORKTREE_ROOT}/code     # UNIT-PROFILE-SEAM-1: capability.h / unitprofile.h
    ${MC2_WORKTREE_ROOT}/3rdparty/include  # MINIMAL-VIABLE-HARNESS-1: zlib.h (ffile/packet)
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# MINIMAL-VIABLE-HARNESS-1: ffile.cpp/packet.cpp call zlib (compress2/uncompress).
# 3rdparty/lib/x64/zlib.lib is the static build -- no zlib1.dll needed at run time.
target_link_libraries(mc2_tests PRIVATE
    ${MC2_WORKTREE_ROOT}/3rdparty/lib/x64/zlib.lib
)

# Match main build's preprocessor environment so engine headers parse the
# same way they do in production. LINUX_BUILD is the load-bearing one
# (memory/mc2_path_separator_linux_build.md); the engine builds with it
# globally on Windows too.
target_compile_definitions(mc2_tests PRIVATE
    LINUX_BUILD
    _CRT_SECURE_NO_WARNINGS
    # XFORM-CONVENTION-HARNESS-1: mclib/xform_conventions.{h,cpp} pulls the
    # Stuff::Matrix4D header chain (stuff.hpp -> gameos.hpp), which selects its
    # platform branch on PLATFORM_WINDOWS and disables the x86 inline-asm matrix
    # path on USE_ASSEMBLER_CODE=0 -- both match the main build's global defines
    # (CMakeLists.txt:62,136). Header-include only; no GL context is created.
    PLATFORM_WINDOWS
    USE_ASSEMBLER_CODE=0
    # MINIMAL-VIABLE-HARNESS-1: on-disk fixtures for the file-stack tests.
    MC2_TEST_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
)

if(MSVC)
    target_compile_options(mc2_tests PRIVATE /W4 /WX-)
endif()

# Make `cmake --build ... --target check` run the tests after build.
add_custom_target(check
    COMMAND mc2_tests
    DEPENDS mc2_tests
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Running mc2 unit tests"
)

# Convenience target: run only the RenderCore test suite.
add_custom_target(rendercore_tests
    COMMAND mc2_tests --ts=RenderCore
    DEPENDS mc2_tests
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Running RenderCore substrate unit tests"
)

# CONTRACT-3: per-frame resource-ordering tracking test.
# Standalone binary (own int main, no doctest) because render_contract.cpp
# carries a GL include that the main mc2_tests target deliberately avoids.
# Links glew32 for compilation; CONTRACT-3 paths never call GL at runtime.
add_executable(test_render_contract_3
    test_render_contract_3.cpp
    ${MC2_WORKTREE_ROOT}/mclib/render_contract.cpp
    ${MC2_WORKTREE_ROOT}/RenderCore/RenderResourceRegistry.cpp
)

target_include_directories(test_render_contract_3 PRIVATE
    ${MC2_WORKTREE_ROOT}           # for "RenderCore/RenderPassContract.h" style includes
    ${MC2_WORKTREE_ROOT}/mclib
    ${MC2_WORKTREE_ROOT}/RenderCore
    ${MC2_WORKTREE_ROOT}/GameOS
    ${MC2_WORKTREE_ROOT}/3rdparty/include
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_definitions(test_render_contract_3 PRIVATE
    LINUX_BUILD
    _CRT_SECURE_NO_WARNINGS
)

if(MSVC)
    target_compile_options(test_render_contract_3 PRIVATE /W4 /WX-)
endif()

target_link_libraries(test_render_contract_3 PRIVATE
    ${MC2_WORKTREE_ROOT}/3rdparty/lib/x64/glew32.lib
    opengl32
)
