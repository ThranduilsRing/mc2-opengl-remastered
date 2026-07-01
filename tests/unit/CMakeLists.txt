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
    test_unit_actions.cpp   # UNITPROFILE-OFFLINE-HARNESS-1: resolveAction + executor registry (offline)
    test_unit_profile_validator.cpp  # UNITPROFILE-VALIDATOR-1: load-time construction-rules validator (offline)
    test_view_currency.cpp  # VIEW-EPOCH-DEDUPE-1: object MVP currency kernel (offline)
    test_frame_graph.cpp    # FRAME-GRAPH-SKELETON-1: resource-DAG validation (offline)
    test_gpu_buffer_owner.cpp  # GPU-BUFFER-OWNER-SKELETON-1: owner record invariants (offline)
    test_det_rng.cpp        # DETERMINISTIC-RNG-1: LCG kernel determinism (offline)
    test_vulkan_contract.cpp  # VULKAN-CONTRACT-COVERAGE-1: descriptor-class coverage/consistency (offline)
    test_hardpoint.cpp        # HARDPOINT-ID-SCHEMA-1: HardpointId join key + weapon-tag/damage-location link (offline)
    test_getweaponnode_adapter.cpp  # GETWEAPONNODE-HARDPOINT-ADAPTER-1: chooseWeaponNode() authoritative-with-fallback (offline)
    test_weapon_definition.cpp  # WEAPONDEF-SCHEMA-1: WeaponFacet (was WeaponDefinition) facets + WeaponId string-id identity + weaponFitsHardpoint join (offline)
    test_equipment_definition.cpp  # EQUIPMENT-DEFINITION-SCHEMA-1: EquipmentDefinition top type (kind/tags/legacyMasterId-alias/facet slots) + WeaponFacet fold-in + equipmentFitsHardpoint (offline)
    test_nonmech_data_unit_proof.cpp  # NON-MECH-DATA-UNIT-PROOF-1: data-driven mount route + UI override + validateUnitDefinition coherence (offline)
    test_action_anim_tag.cpp  # ACTION-ANIMATION-BINDINGS-1: ActionAnimTag -> gesture-id resolver + fallback chains (offline)
    test_ui_profile.cpp       # UI-PROFILE-1: UIProfile facet + accessor-override helpers + UPV_UI_ASSET_MISSING (offline)
    test_unit_definition.cpp  # SINGLE-SOURCE-UNIT-DEFINITION-1: UnitDefinition facade + mergeUIProfile single UI resolution (offline)
    test_facet_names.cpp      # UNIT-DEFINITION-SHADOW-COMPARE-1: locomotion/damage token-name emitters + parse round-trip (offline)
    test_modlint.cpp          # UNIT-MODLINT-CLI-1: reason-code->message table + facts-JSON parse + formatter (offline)
    test_qualified_id.cpp     # UNIT-ID-NAMESPACE-1: namespace:name convention + charset + deterministic override precedence (offline)
    test_weapon_events.cpp    # WEAPON-FACET-PRESENTATION-SCHEMA: WeaponFacet.fireExecutor binding + WeaponEvent->asset presentation grid + flat back-compat (offline)
    test_equipment_validator.cpp  # WEAPON-VALIDATOR-1: pure construction-rules validator for EquipmentDefinition/WeaponFacet (EQV_* findings) (offline)
    test_equipment_modlint.cpp    # WEAPON-MODLINT-1: EQV_ reason-code->message table + equipment facts-JSON parse + converter-roster parse + formatter (offline)
    test_equipment_registry.cpp   # EQUIPMENT-REGISTRY-FACADE-1: pure slug (mirrors converter _slug) + form->kind + registry query API (offline)
    test_equipment_id_alias.cpp   # LOADOUT-EQUIPMENT-ID-ALIAS-1 (E5): resolveLoadoutEquipmentRef qualified/numeric/not-found/needs-widen (offline)
    test_fire_executor_registry.cpp  # FIRE-EXECUTOR-REGISTRY-1 (E7): bind/isBound/entryFor/resolve + stock executor set + classify unset/unbound (offline)

    # Leaf-TU engine sources (no transitive engine include drag).
    ${MC2_WORKTREE_ROOT}/mclib/fst_hash.cpp
    ${MC2_WORKTREE_ROOT}/mclib/xform_conventions.cpp  # XFORM-CONVENTION-HARNESS-1: GL-free leaf TU
    ${MC2_WORKTREE_ROOT}/RenderCore/RenderResourceRegistry.cpp
    ${MC2_WORKTREE_ROOT}/RenderCore/RenderDebugView.cpp
    ${MC2_WORKTREE_ROOT}/HostServices/DefaultHostServices.cpp  # ENGINE-HOST-SERVICES-0
    ${MC2_WORKTREE_ROOT}/code/tacticaloverview_state.cpp  # TACTICAL-OVERVIEW-1
    ${MC2_WORKTREE_ROOT}/code/unitprofile.cpp   # UNIT-PROFILE-SEAM-1
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
    ${CMAKE_CURRENT_SOURCE_DIR}
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

# UNIT-MODLINT-CLI-1 (Phase-3 slice 3): the offline modder validator CLI.
# Standalone binary that reads a unit facts-JSON, runs the AUTHORITATIVE pure
# validateUnitDefinition() (single source of truth, shared with mc2_tests + the
# engine), and prints modder-readable findings with a CI exit code. GL/IO-free
# like mc2_tests — the only IO is reading the input file. Built from the SAME
# standalone cmake as mc2_tests (build64-tests). No engine, no OpenGL link.
add_executable(mc2_modlint
    ${MC2_WORKTREE_ROOT}/tools/mc2_modlint/mc2_modlint.cpp
)

target_include_directories(mc2_modlint PRIVATE
    ${MC2_WORKTREE_ROOT}/code             # unit_profile_validator.h + facet headers
    ${MC2_WORKTREE_ROOT}/tools/mc2_modlint
)

target_compile_definitions(mc2_modlint PRIVATE
    LINUX_BUILD
    _CRT_SECURE_NO_WARNINGS
)

if(MSVC)
    target_compile_options(mc2_modlint PRIVATE /W4 /WX-)
endif()
