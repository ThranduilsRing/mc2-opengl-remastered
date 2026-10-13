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

include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/Mc2Standalone.cmake")

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
    test_gate.cpp           # GATE-REGISTRY-G1A: truthiness matrix, slot cache, [CONFIG v1] dump
    test_tactical_overview.cpp   # TACTICAL-OVERVIEW-1: blend-state pure logic
    test_xform_convention.cpp    # XFORM-CONVENTION-HARNESS-1: clip-space convention lock
    test_capability.cpp     # UNIT-PROFILE-SEAM-1
    test_unit_actions.cpp   # UNITPROFILE-OFFLINE-HARNESS-1: resolveAction + executor registry (offline)
    test_unit_profile_validator.cpp  # UNITPROFILE-VALIDATOR-1: load-time construction-rules validator (offline)
    test_view_currency.cpp  # VIEW-EPOCH-DEDUPE-1: object MVP currency kernel (offline)
    test_frame_graph.cpp    # FRAME-GRAPH-SKELETON-1: resource-DAG validation (offline)
    test_gpu_buffer_owner.cpp  # GPU-BUFFER-OWNER-SKELETON-1: owner record invariants (offline)
    test_det_rng.cpp        # DETERMINISTIC-RNG-1: LCG kernel determinism (offline)
    test_appearance_rng.cpp # the appearance-side random stream: replayable, in range, apart from the simulation's
    test_script_music.cpp   # the music state an ABL script sees: its own request on the scenario clock, never the device
    alloc_probe.cpp         # the allocation probe + ScopedLeakCheck, proven on itself and on EString (step 4 leak oracle)
    alloc_probe_new.cpp     # this binary's global operator new/delete, routed through the probe
    test_saved_objects_check.cpp  # SAVE-REJECT-1: object-packet run check of an in-mission save (offline)
    test_save_struct_zero.cpp  # SAVE-ZERO-1: MC2_SAVE_STRUCT zeroes struct padding on a dirty stack (offline)
    test_sim_identity.cpp   # SIM-IDENTITY-HASH-1: world-hash order/quantum kernel
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
    test_modlint_manifest.cpp # MOD-PACKAGE-MANIFEST-1: mod.json manifest MODMAN_* rules + formatter (offline)
    test_mod_conflict.cpp     # MOD-LOAD-ORDER-CONFLICT-REPORT-1: pure conflict classifier + formatter (offline)
    test_variant_csv_modlint.cpp # UNIT-VARIANT-ADD-1 (S4): legacy chassis-CSV variant-block validator (VARC_ rules: stride/unreachable/dup-name/loadout-refs/truncation) (offline)
    test_modder_finding.cpp   # MODDER-FIRST-ERROR-SURFACE-1: shared finding struct + one-line context format + silent-fallback token probes (offline)
    test_qualified_id.cpp     # UNIT-ID-NAMESPACE-1: namespace:name convention + charset + deterministic override precedence (offline)
    test_weapon_events.cpp    # WEAPON-FACET-PRESENTATION-SCHEMA: WeaponFacet.fireExecutor binding + WeaponEvent->asset presentation grid + flat back-compat (offline)
    test_equipment_validator.cpp  # WEAPON-VALIDATOR-1: pure construction-rules validator for EquipmentDefinition/WeaponFacet (EQV_* findings) (offline)
    test_equipment_modlint.cpp    # WEAPON-MODLINT-1: EQV_ reason-code->message table + equipment facts-JSON parse + converter-roster parse + formatter (offline)
    test_equipment_registry.cpp   # EQUIPMENT-REGISTRY-FACADE-1: pure slug (mirrors converter _slug) + form->kind + registry query API (offline)
    test_equipment_id_alias.cpp   # LOADOUT-EQUIPMENT-ID-ALIAS-1 (E5): resolveLoadoutEquipmentRef qualified/numeric/not-found/needs-widen (offline)
    test_fire_executor_registry.cpp  # FIRE-EXECUTOR-REGISTRY-1 (E7): bind/isBound/entryFor/resolve + stock executor set + classify unset/unbound (offline)
    test_weapon_presentation_resolve.cpp  # ONE-WEAPON-PRESENTATION-PROOF-1 (E8): pure OnFire/OnMuzzleFlash data-vs-legacy effect-id resolution + asset->id parse (offline)
    test_unit_source_flip.cpp  # UNIT-SOURCE-FLIP-SWITCH-1: MC2_UNIT_DEFINITION_SOURCE gate parse + variant-slug + generated-token facet apply (offline)
    test_equipment_source_flip.cpp  # EQUIPMENT-SOURCE-FLIP-SWITCH-1: MC2_EQUIPMENT_DEFINITION_SOURCE gate parse + range-bucket->ordinal + per-field overlay fallback (offline)
    test_equipment_add_fill.cpp  # EQUIP-ADD-FILL-1 (S2): gate parse + ns-aware def path + fill decision matrix + kind->form + AllowedLocations->crit + dual-consumer CSV line lock + EQV_ADD_* validator mirrors (offline)
    test_mod_cache_sig.cpp       # MOD-INDEX-CACHE-MTIME-1: mod-index cache validity-signature kernel (sig-line parse/format + match rule, offline)
    test_vulkan_layout_chain.cpp  # VULKAN-LAYOUT-CHAIN-VALIDATOR-2: sampled-image layout-chain coverage/producer (offline)
    test_subgraph_contiguity.cpp  # SUBGRAPH-CONTIGUITY-GUARD-1: postprocess sub-pass contiguity rule (offline)
    test_file_inifile.cpp   # MINIMAL-VIABLE-HARNESS-1: real file/inifile/packet stack, game-free
    test_path_permutations.cpp  # META-FIX design E: canonical-key permutation matrix (issue #8 family)
    test_csvfile.cpp        # MINIMAL-VIABLE-HARNESS-2: real csvfile.cpp (Buildings.csv-shaped)
    test_memorystream.cpp   # Stuff::MemoryStream::ReadBytes bounds check (truncated mod .fx)
    test_estring.cpp        # EString::Format sizing (ll/z/j specs) via the CRT parser; step 4 B1 characterization of the class (H1-H14)
    test_objective_text.cpp # objective text reader/writer + boolean flag table (step 4 B1.1)
    test_gui_bridge.cpp     # widget text bridge: setText / setEntry overload resolution (compile-time, step 4 B1)
    test_mc2str.cpp         # mc2::str helpers (from_c, ASCII-fold equality/order, ascii_lower/upper) + the stock-identifier corpus against the old compare
    test_abl_concat.cpp     # ABL concat(): the append never writes past the script variable's declared size
    test_abl_position.cpp   # ABL getRelativePositionToObject: the REAL binding against a scripted argument queue (abl_exec_shim.cpp)
    abl_exec_shim.cpp       # the interpreter stack / object table the position bindings read, answered from the scripted queue
    ${MC2_WORKTREE_ROOT}/mclib/abl_script_events.cpp  # the ABL reject/clamp log lines (leaf: reads CurModule / execLineNumber, answered by abl_exec_shim.cpp)
    ${MC2_WORKTREE_ROOT}/code/ablmc2_position.cpp  # the ABL position builtins under test (a TU of their own so they link without ablmc2_misc.cpp)
    test_fst_roundtrip.cpp  # MINIMAL-VIABLE-HARNESS-2: FST create/writeFast -> loose-vs-FST ladder
    test_paths_mod.cpp      # MINIMAL-VIABLE-HARNESS-2: paths.cpp globals + mod-index (TryModOpen) resolution
    test_pilot_profile_preserve.cpp  # PILOT-PROFILE-PRESERVE-1: editor Profile save decision kernel (offline)
    test_ui_defs_lockstep.cpp  # UI-DEFS-LOCKSTEP-HARDEN-1: defs/ui package version + zero-element/zero-draw fail-soft (offline)
    test_hud_tab_registry.cpp  # HUD-IMGUI slice C: tab registry order/cycle invariants (offline)
    test_infowindow_model.cpp  # HUD-IMGUI slice D: info-window component aggregation kernel (offline)
    test_terrain_material.cpp  # TERRAIN-MATERIAL-SINGLE-SOURCE: terrain type -> material table (type 20 = Concrete)
    test_mc2log.cpp         # LOGGING-FACADE-P0: mc2::log facade (level gate, vformat fail-soft, MC2_EVENT bytes, MC2_LOG spec)
    test_mc2fmt.cpp         # STEP3-LOGGING-B0: mc2fmt.h to_buf/to_buf_n/safe/as_* vs snprintf (offline)
    test_printf_corpus.cpp  # STEP3-LOGGING-B0: generated format corpus, printf vs mc2::fmt::to_buf byte equality
    ${MC2_WORKTREE_ROOT}/GameOS/gameos/utils/mc2log.cpp  # LOGGING-FACADE-P0: leaf TU (no engine deps)

    # MINIMAL-VIABLE-HARNESS-1: malloc-backed heap + GameOS-global shim.
    # Replaces heap.cpp (won't compile standalone -- see
    # docs/testing/fitini-inmem-harness-bootstrap-recon-1.md) and
    # gameos.cpp/gameos_debugging.cpp (Environment / InternalFunctionStop).
    # heap.cpp MUST NOT be added below while this shim is linked (ODR).
    heap_shim.cpp

    # Leaf-TU engine sources (no transitive engine include drag).
    ${MC2_WORKTREE_ROOT}/mclib/fst_hash.cpp
    ${MC2_WORKTREE_ROOT}/mclib/appearance_rng.cpp
    ${MC2_WORKTREE_ROOT}/mclib/alloc_probe.cpp       # allocation accounting seam (portable, proof builds + this tier)
    ${MC2_WORKTREE_ROOT}/mclib/xform_conventions.cpp  # XFORM-CONVENTION-HARNESS-1: GL-free leaf TU
    ${MC2_WORKTREE_ROOT}/RenderCore/RenderResourceRegistry.cpp
    ${MC2_WORKTREE_ROOT}/RenderCore/RenderDebugView.cpp
    ${MC2_WORKTREE_ROOT}/code/tacticaloverview_state.cpp  # TACTICAL-OVERVIEW-1
    ${MC2_WORKTREE_ROOT}/code/unitprofile.cpp   # UNIT-PROFILE-SEAM-1
    ${MC2_WORKTREE_ROOT}/code/hudtabregistry.cpp  # HUD-IMGUI slice C (pure leaf TU)

    # MINIMAL-VIABLE-HARNESS-1: the real file/ini/packet stack. This is the
    # measured dependency frontier for file.cpp + inifile.cpp (every TU below
    # is required at link time; nothing else from mclib/GameOS is):
    ${MC2_WORKTREE_ROOT}/mclib/file.cpp       # File base: disk/FST/in-RAM backends
    ${MC2_WORKTREE_ROOT}/mclib/inifile.cpp    # FitIniFile block/var parser
    ${MC2_WORKTREE_ROOT}/mclib/inifile_read.cpp
    ${MC2_WORKTREE_ROOT}/mclib/inifile_arrays.cpp
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
    ${MC2_WORKTREE_ROOT}/mclib/stuff/memorystream.cpp      # Stuff::MemoryStream (gosFX .fx reader)
    ${MC2_WORKTREE_ROOT}/mclib/stuff/registeredclass.cpp   # its RegisteredClass base
    ${MC2_WORKTREE_ROOT}/mclib/stuff/scalar.cpp            # Scaled_*_Bits used by its bit I/O
    ${MC2_WORKTREE_ROOT}/mclib/stuff/mstring.cpp           # MString used by its string I/O
    ${MC2_WORKTREE_ROOT}/mclib/stuff/style.cpp             # Stuff::SMALL and the other global constants
    ${MC2_WORKTREE_ROOT}/mclib/estring.cpp                 # EString under test
    ${MC2_WORKTREE_ROOT}/code/objective_text.cpp           # objective string reader/writer (leaf: FitIniFile + log facade)
    ${MC2_WORKTREE_ROOT}/code/objective_flags.cpp          # objective boolean flag table (leaf)
    ${MC2_WORKTREE_ROOT}/mclib/mlr/mlr_gate.cpp            # MC2_DISABLE_GOSFX latch

    # MINIMAL-VIABLE-HARNESS-2: paths.cpp is a pure leaf TU (path string globals
    # only -- no TryModOpen/mod-index; that logic lives in file.cpp, already
    # linked above). Linking it lets tests reference the engine's real path
    # globals (objectPath/artPath/...) instead of stand-ins.
    ${MC2_WORKTREE_ROOT}/mclib/paths.cpp      # path string globals (leaf)
)
mc2_link_gate(mc2_tests)

# The ABL native-binding headers (ablmc2_internal.h) pull the gui/ and netlib/ headers; only the TUs that include them get those directories.
set_source_files_properties(test_abl_position.cpp test_gui_bridge.cpp abl_exec_shim.cpp ${MC2_WORKTREE_ROOT}/code/ablmc2_position.cpp ${MC2_WORKTREE_ROOT}/mclib/abl_script_events.cpp PROPERTIES
    INCLUDE_DIRECTORIES "${MC2_WORKTREE_ROOT}/gui;${MC2_WORKTREE_ROOT}/netlib;${MC2_WORKTREE_ROOT}/3rdparty/tracy")

# LOGGING-FACADE-P0: mc2log.h needs <format>/consteval (C++20); engine headers already build at 23.
set_target_properties(mc2_tests PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)

# The generated printf corpus (one TEST_CASE per converted format pair, thousands of them) exceeds the 65279-section limit of a normal object.
set_source_files_properties(test_printf_corpus.cpp PROPERTIES COMPILE_FLAGS "/bigobj")

target_include_directories(mc2_tests PRIVATE
    ${MC2_WORKTREE_ROOT}           # for "RenderCore/RenderPassContract.h" style includes
    ${MC2_WORKTREE_ROOT}/mclib
    ${MC2_WORKTREE_ROOT}/GameOS
    ${MC2_WORKTREE_ROOT}/GameOS/include   # XFORM-CONVENTION-HARNESS-1: platform_str.h / gameos.hpp for stuff.hpp chain
    ${MC2_WORKTREE_ROOT}/GameOS/gameos    # XFORM-CONVENTION-HARNESS-1: utils/vec.h pulled by gameos.hpp
    ${MC2_WORKTREE_ROOT}/RenderWorld
    ${MC2_WORKTREE_ROOT}/RenderCore
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
mc2_asan_runtime(mc2_tests)

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
    # DARK-TESTS-RUN-1: render_contract.cpp forwards two PUBLIC apply-state
    # counters onto *_impl symbols defined one layer up in
    # GameOS/gameos/gos_postprocess.cpp -- a ~7k-line TU with the whole
    # renderer behind it. Any STANDALONE link of render_contract.cpp therefore
    # died on LNK2019 x2. This target had carried that break for as long as
    # nobody built it; gos_counter_stubs.cpp's own header NOTE predicted it
    # ("currently green by omission ... building it will need this same stub")
    # and this lane confirmed it by actually running the link. Same stub TU the
    # render_state_contract_harness already uses -- no new stub, no fake.
    ${MC2_WORKTREE_ROOT}/tools/render_state_contract_harness/gos_counter_stubs.cpp
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
# render_contract.cpp reads its gates through the typed registry.
mc2_link_gate(test_render_contract_3)

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
    ${MC2_WORKTREE_ROOT}/mclib            # mod_conflict.h (shared pure conflict classifier)
    ${MC2_WORKTREE_ROOT}/tools/mc2_modlint
)

target_compile_definitions(mc2_modlint PRIVATE
    LINUX_BUILD
    _CRT_SECURE_NO_WARNINGS
)

if(MSVC)
    target_compile_options(mc2_modlint PRIVATE /W4 /WX-)
endif()

# The former contract executables share doctest; the GL guard cases retain a real hidden context.
target_sources(mc2_tests PRIVATE
    test_contract_smoke_harness.cpp
    test_shader_contract_harness.cpp
    test_render_state_contract_harness.cpp
    test_objmgr_contract_harness.cpp
    test_ibl_registry_contract_harness.cpp
    test_render_pass_table_harness.cpp
    test_logistics_csv_harness.cpp
    test_camera_frustum_harness.cpp
    test_mech_texname_harness.cpp
    test_icon_atlas_harness.cpp
    test_timing_math_harness.cpp
    test_job_system_harness.cpp
    test_terrain_daylight_harness.cpp
    test_object_light_row_harness.cpp
    test_class_light_band_harness.cpp
    test_glstate_guard_harness.cpp
    ${MC2_WORKTREE_ROOT}/mclib/render_contract.cpp
    ${MC2_WORKTREE_ROOT}/tools/render_state_contract_harness/gos_counter_stubs.cpp
    ${MC2_WORKTREE_ROOT}/code/job_system.cpp
    ${MC2_WORKTREE_ROOT}/code/jobs_fatal.cpp
)
target_compile_features(mc2_tests PRIVATE cxx_std_23)
target_compile_definitions(mc2_tests PRIVATE MC2_REPO_ROOT="${MC2_WORKTREE_ROOT}")
target_include_directories(mc2_tests PRIVATE
    ${MC2_WORKTREE_ROOT}/GameOS/gameos/utils
    ${MC2_WORKTREE_ROOT}/3rdparty/include/SDL2
)
find_package(Threads REQUIRED)
target_link_libraries(mc2_tests PRIVATE
    Threads::Threads
    ${MC2_WORKTREE_ROOT}/3rdparty/lib/x64/SDL2.lib
    ${MC2_WORKTREE_ROOT}/3rdparty/lib/x64/glew32.lib
    opengl32
)
foreach(_contract_dll SDL2.dll glew32.dll)
    add_custom_command(TARGET mc2_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${MC2_WORKTREE_ROOT}/3rdparty/lib/x64/${_contract_dll}"
            "$<TARGET_FILE_DIR:mc2_tests>/${_contract_dll}"
        COMMENT "Copy ${_contract_dll} for hidden GL guard tests"
    )
endforeach()
