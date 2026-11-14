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

set_source_files_properties(${MC2_WORKTREE_ROOT}/RenderCore/KtxBasis.cpp PROPERTIES COMPILE_DEFINITIONS NDEBUG)  # vendored basisu asserts do not compile under clang
add_executable(mc2_tests
    test_fog_policy.cpp
    test_water_reactive.cpp
    test_fx_native.cpp
    ${MC2_WORKTREE_ROOT}/mclib/fx/fx_render.cpp
    ${MC2_WORKTREE_ROOT}/mclib/fx/fx_format.cpp
    ${MC2_WORKTREE_ROOT}/mclib/fx/fx_catalog.cpp
    ${MC2_WORKTREE_ROOT}/mclib/fx/fx_curve.cpp
    ${MC2_WORKTREE_ROOT}/mclib/fx/fx_mesh_data.cpp
    test_support_craft.cpp
    test_jump_target.cpp
    test_path_work.cpp
    test_contact_pick.cpp
    test_building_static_eligibility.cpp
    test_terrain_dirty_queue.cpp
    test_sim_tick_cut.cpp  # SIM-TICK-CUT: terrain tick list + occupied-cell collision walk match the legacy scans
    test_present_handoff.cpp
    test_terrain_record_cache.cpp  # H8 terrain-submit-cut: record cache (static camera 0 rebuilds, one dirty = one)
    test_present_wait.cpp  # PRESENT-HARDEN: pumped bounded main waits, failure seen at seal, start-spaced cap pacing
    test_commander_team.cpp # COMMANDER-TEAM: commanderPartTeam fallback (first part wins, team 0, non-home team, no parts)
    test_pilot_kill_result.cpp
    test_soft_splash.cpp
    test_cursor_map.cpp    # CURSOR-SCALE-1: logical 2D space -> ImGui display mapping of the software cursor
    test_visual_capture_live_settle.cpp  # CAPTURE-LIVE-CLOCK: pose -> run N sim-s -> pause sequencing
    ${MC2_WORKTREE_ROOT}/3rdparty/imgui/imgui.cpp
    ${MC2_WORKTREE_ROOT}/3rdparty/imgui/imgui_draw.cpp
    ${MC2_WORKTREE_ROOT}/3rdparty/imgui/imgui_tables.cpp
    ${MC2_WORKTREE_ROOT}/3rdparty/imgui/imgui_widgets.cpp
    test_runtime_observation.cpp
    test_runtime_bridge_protocol.cpp
    ${MC2_WORKTREE_ROOT}/GameOS/gameos/runtime_bridge_protocol.cpp
    test_building_gameplay_observation.cpp
    test_main.cpp           # owns DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
    test_abl_callback_address.cpp
    test_crash_bundle.cpp
    test_mission_environment.cpp
    ${MC2_WORKTREE_ROOT}/GameOS/gameos/visual_tuning_profile.cpp
    ${MC2_WORKTREE_ROOT}/GameAdapters/MissionEnvironmentAdapter.cpp
    test_hashing.cpp
    test_projection.cpp     # currently scope-stopped; placeholder skipped case
    test_argb_pack.cpp
    test_mech_tangent_sign.cpp
    test_mover_los_epoch.cpp
    test_screen_pick.cpp
    test_camera_start_pick.cpp
    test_spawn_tree_clear.cpp
    test_quality_presets.cpp
    test_rendercore.cpp     # RENDERCORE-UNIT-TESTS-1: GL-free substrate invariants
    test_frame_arena.cpp    # FRAME-ARENA-1: FrameArena typed L2 allocator invariants
    test_depth_hzb.cpp      # HZB-DEPTH-CONVENTION-TESTS-1: reverse-Z / HZB reduction contract
    test_hzb_cull_reference.cpp  # CPU reference of hzb_cull.comp (RenderCore/hzb_prop_bounds.h): keep-bit cases
    test_prop_light_contract.cpp
    test_water_planar.cpp
    test_clip_sample.cpp  # glTF clip sampling: STEP / LINEAR / CUBICSPLINE (header-only, no Assimp)
    test_height_sig.cpp  # gameplay_height.sig hash equals the Python writer's test vector (header-only)
    test_render_snap.cpp  # UNIT-VISUAL-SNAP-2 render-only effect-anchor and selection-box lift (header-only)
    test_shadow_bake_math.cpp  # the static shadow bake's mask layout, reach, footprint and caster-window math (header-only, no Vulkan)
    test_shadow_mover_fit.cpp
    test_static_caster_set.cpp
    ../../GameOS/gameos/static_caster_set.cpp
    ${MC2_WORKTREE_ROOT}/GameOS/gameos/diagnostic_trace.cpp
    test_gate.cpp           # GATE-REGISTRY-G1A: truthiness matrix, slot cache, [CONFIG v1] dump
    test_tactical_overview.cpp   # TACTICAL-OVERVIEW-1: blend-state pure logic
    test_xform_convention.cpp    # XFORM-CONVENTION-HARNESS-1: clip-space convention lock
    test_environment_gpu.cpp     # U1: the environment block SH basis (Y-up data, MC2 up -> zenith) and the one producer
    ${MC2_WORKTREE_ROOT}/GameOS/gameos/mission_environment_gpu.cpp
    test_mech_up_axis.cpp        # mech-up-axis: which component of the mech/prop world normal is up
    test_static_prop_origin.cpp  # prop instance origin layout: translation at [3][7][11]
    test_prop_record_cache.cpp  # prop scene generations and per-frame-slot cache keys
    test_prop_lod_state.cpp  # flat per-instance LOD hysteresis table equals the unordered_map it replaced
    test_searchlight_aim.cpp
    test_cone_beam_links.cpp
    test_beam_lights.cpp
    test_deferred_submit_batch.cpp  # submit-order-guard: reserved recordings vs prefix flush vs the present worker (offline)
    test_capability.cpp     # UNIT-PROFILE-SEAM-1
    test_unit_actions.cpp   # UNITPROFILE-OFFLINE-HARNESS-1: resolveAction + executor registry (offline)
    test_unit_profile_validator.cpp  # UNITPROFILE-VALIDATOR-1: load-time construction-rules validator (offline)
    test_view_currency.cpp  # VIEW-EPOCH-DEDUPE-1: object MVP currency kernel (offline)
    test_temporal_inputs.cpp  # temporal track slice B: jitter sequence, neutral path, MV algebra (offline)
    test_upscaler_provider.cpp  # upscaler provider registry, fallback order, quality-mode math (offline)
    test_frame_graph.cpp    # FRAME-GRAPH-SKELETON-1: resource-DAG validation (offline)
    test_vk_resource_graph.cpp
    test_vk_barrier_batch.cpp  # sync2 barrier batch + per-image tracker: previous use is the source (offline, no Vulkan)
    test_completion_tracker.cpp  # the Vulkan completion tracker: ticket math and frame sealing (offline)
    test_deferred_destroy.cpp  # the Vulkan deferred-destroy queue: not before the ticket, after it, negative control (offline)
    test_frame_ring.cpp  # the Vulkan frame ring offset arithmetic: slots, alignment, growth sizing (offline)
    test_record_pools.cpp  # parallel command recording lanes: per-thread pools, canonical merge order, slot restart (offline)
    test_vfx_draw_plan.cpp  # VFX island draw packing: equal-state merge, additive runs sorted, alpha order kept
    test_readback.cpp  # the Vulkan readback ring: ticket, tag and generation checks, wrong-tag negative control (offline)
    test_light_pool_culling.cpp
    test_cpu_run_cache.cpp
    test_explosion_light.cpp
    test_terrain_vertex_dirty.cpp
    test_light_feed_sync.cpp  # LIGHTING-CPU-CUT: light-feed slot sync (revision fast exit + journal) equals the source rows
    test_local_lights.cpp   # LOCAL-LIGHTS: pool packing vs shader decode, shadow-tile selection, mission light profile (offline)
    test_texture_service.cpp  # P4a texture/material identity: key dedupe, mod precedence, paint variants, stale handles, material sets (offline)
    test_visibility_history.cpp # native actor visibility: paired IDs, lagged tiers, OR, stale fail-open
    test_texture_sources.cpp    # session source admission, strong refs, mission scopes, no renderer
    test_tick_commit.cpp
    test_actor_pool.cpp  # session-owned typed actor pool: spawn/retire/reuse, stale IDs, render-packet deferred destruction (offline)
    ${MC2_WORKTREE_ROOT}/code/tick_commit.cpp
    test_playmovie_deferral.cpp  # VIDEO-DECODE-OFFLINE-1: real ControlGui::playMovie + MC2Movie::init on the real overlap worker, decoder seam only
    ${MC2_WORKTREE_ROOT}/code/controlgui_chat.cpp
    ${MC2_WORKTREE_ROOT}/code/mc2movie.cpp
    ${MC2_WORKTREE_ROOT}/code/render_view_store.cpp
    ${MC2_WORKTREE_ROOT}/mclib/texture_sources.cpp
    test_handle_slots.cpp  # P3 handle slots: ticket-gated reuse, stale handles refused, generation wrap, range exhaustion (offline)
    test_world_registry_grid.cpp
    test_frame_packets.cpp  # P1 frame packets: publish freezes a frame's views and lighting, per-slot arenas, republish and recycle (offline)
    test_det_rng.cpp        # DETERMINISTIC-RNG-1: LCG kernel determinism (offline)
    test_arm_node_remap.cpp # arm-off weapon-node remap: valid node on the remaining body (named sim change)
    test_appearance_rng.cpp # the appearance-side random stream: replayable, in range, apart from the simulation's
    test_script_music.cpp   # the music state an ABL script sees: its own request on the scenario clock, never the device
    alloc_probe.cpp         # the allocation probe + ScopedLeakCheck, allocation accounting and standard-owner coverage
    alloc_probe_new.cpp     # this binary's global operator new/delete, routed through the probe
    test_saved_objects_check.cpp  # SAVE-REJECT-1: object-packet run check of an in-mission save (offline)
    test_save_struct_zero.cpp  # SAVE-ZERO-1: MC2_SAVE_STRUCT zeroes struct padding on a dirty stack (offline)
    test_fit_number.cpp     # FIT-NUMBER-1: the FIT number readers shared with the editor keep the game's quirks
    test_save_pointer_pins.cpp  # SAVE-PTR-PIN-1: Gate closestObject and QueuedTacOrder marker are written as null (source pins)
    test_sim_identity.cpp   # SIM-IDENTITY-HASH-1: world-hash order/quantum kernel
    test_anim_binding_cache.cpp
    test_msl_node_walk.cpp  # MSL-NODE-WALK-1: keyed node rebuild equals a full rebuild bit for bit (header-only, mclib/msl_node_walk.h)
    test_vulkan_contract.cpp  # VULKAN-CONTRACT-COVERAGE-1: descriptor-class coverage/consistency (offline)
    test_veg_hardsurface.cpp  # VEG-CEMENT-1: cement coverage -> vegetation falloff curve, crack weeds, class-3 union (offline)
    test_prop_array_buckets.cpp  # PROP-ARRAY-BUCKETS: alpha-ON prop albedo size buckets + (bucket, layer) encoding (offline)
    test_startup_work.cpp  # PC night startup: bounded dedup, appearance noise, TGA scratch reuse
    test_hardpoint.cpp        # HARDPOINT-ID-SCHEMA-1: HardpointId join key + weapon-tag/damage-location link (offline)
    test_package_chassis.cpp  # BT2018-FIRST-CLASS P3/P7: package chassis regions/mounts/payload + type-number allocation (offline)
    test_mech_clip_state.cpp  # BT2018-FIRST-CLASS P9: imported-mech clip state machine + stride matching (offline)
    test_mech_wreck.cpp
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
    test_impact_decals.cpp  # weapon impact decals: event->recipe mapping, count budget, merge/fade/heat, anchors, GPU box, atlas
    test_weapon_presentation_resolve.cpp  # ONE-WEAPON-PRESENTATION-PROOF-1 (E8): pure OnFire/OnMuzzleFlash data-vs-legacy effect-id resolution + asset->id parse (offline)
    test_unit_source_flip.cpp  # UNIT-SOURCE-FLIP-SWITCH-1: MC2_UNIT_DEFINITION_SOURCE gate parse + variant-slug + generated-token facet apply (offline)
    test_equipment_source_flip.cpp  # EQUIPMENT-SOURCE-FLIP-SWITCH-1: MC2_EQUIPMENT_DEFINITION_SOURCE gate parse + range-bucket->ordinal + per-field overlay fallback (offline)
    test_equipment_add_fill.cpp  # EQUIP-ADD-FILL-1 (S2): gate parse + ns-aware def path + fill decision matrix + kind->form + AllowedLocations->crit + dual-consumer CSV line lock + EQV_ADD_* validator mirrors (offline)
    test_mod_cache_sig.cpp       # MOD-INDEX-CACHE-MTIME-1: mod-index cache validity-signature kernel (sig-line parse/format + match rule, offline)
    test_vulkan_layout_chain.cpp  # VULKAN-LAYOUT-CHAIN-VALIDATOR-2: sampled-image layout-chain coverage/producer (offline)
    test_subgraph_contiguity.cpp  # SUBGRAPH-CONTIGUITY-GUARD-1: postprocess sub-pass contiguity rule (offline)
    test_file_inifile.cpp   # MINIMAL-VIABLE-HARNESS-1: real file/inifile/packet stack, game-free
    ${MC2_WORKTREE_ROOT}/mclib/ablfile.cpp
    test_file_family_contract.cpp   # step 4 B3.0: the File family's name/child/buffer contract before B3.1
    test_pak_read_exact.cpp     # step 4 B4.1: mc2::pak::readExact, the size-checked packet read of the move maps (malformed pak rejected with one log line)
    test_tga_scratch.cpp        # step 4 B4.2a: the whole-file read and the TGA body-size check of the terrain texture loaders
    test_globalmap_limits.cpp   # step 4 B4.1c: the range checks of every count a global move map reads from a pak, plus the zeroed owner helper
    test_terrain_part_chunk.cpp
    test_texture_ref.cpp        # step 4 B5.1: mc2::BasicTextureRef, one release per adopted texture node reference
    test_prefs_init.cpp         # deterministic preference init: options.cfg x orgprefs.cfg presence matrix and resulting bytes
    test_path_permutations.cpp  # META-FIX design E: canonical-key permutation matrix (issue #8 family)
    test_csvfile.cpp        # MINIMAL-VIABLE-HARNESS-2: real csvfile.cpp (Buildings.csv-shaped)
    test_memorystream.cpp   # Stuff::MemoryStream::ReadBytes bounds check (truncated mod .fx)
    test_objective_text.cpp # objective text reader/writer + boolean flag table (step 4 B1.1)
    test_score_sort_ties.cpp    # step 4 B2.4: the CRT qsort tie table recorded from the old SortList (golden/sortlist_ties.h) against the new rule
    test_score_sort.cpp         # step 4 B2.4: mc2::sort_scored, the ordering rule that replaces SortList (SS1-SS7)
    test_pqueue_contract.cpp    # step 4 B2.0/B2.5: the open list (mc2::OpenHeap) against the 2000 golden scripts of the removed PriorityQueue (golden/pqueue_golden.h)
    test_gui_bridge.cpp     # widget text bridge: setText / setEntry overload resolution (compile-time, step 4 B1)
    test_mc2str.cpp         # mc2::str helpers (from_c, ASCII-fold equality/order, ascii_lower/upper) + the stock-identifier corpus against the old compare
    test_resource_strings.cpp
    test_resource_messages.cpp  # typed resource messages over the real catalog (MC2Strings.cpp linked)
    test_abl_include_path.cpp  # ABL #include_ next to the including script: mixed separators, OConst.abi (Windows regression)
    test_abl_concat.cpp     # ABL concat(): the append never writes past the script variable's declared size
    test_abl_position.cpp   # ABL getRelativePositionToObject: the REAL binding against a scripted argument queue (abl_exec_shim.cpp)
    test_cast_census.cpp    # cast census: MString hash (bounded copies, five-bit shift) and ABL double2long keep their results
    test_abl_stack_limit.cpp  # ABL resetStack: the root stack limit is the storage end (abl_exec_shim.cpp)
    abl_exec_shim.cpp       # the interpreter stack / object table the position bindings read, answered from the scripted queue
    ${MC2_WORKTREE_ROOT}/mclib/abl_script_events.cpp  # the ABL reject/clamp log lines (leaf: reads CurModule / execLineNumber, answered by abl_exec_shim.cpp)
    ${MC2_WORKTREE_ROOT}/code/ablmc2_position.cpp  # the ABL position builtins under test (a TU of their own so they link without ablmc2_misc.cpp)
    test_fst_roundtrip.cpp  # MINIMAL-VIABLE-HARNESS-2: FST create/writeFast -> loose-vs-FST ladder
    test_fst_backslash.cpp  # retail backslash/mixed-case FST members canonicalized in memory at open; stock parity
    test_paths_mod.cpp      # MINIMAL-VIABLE-HARNESS-2: paths.cpp globals + mod-index (mc2::vfs::openRead) resolution
    test_mod_regressions.cpp # quick synthetic regressions; no installed mods or game required
    ${MC2_WORKTREE_ROOT}/mclib/model_override_registry.cpp
    test_vfs_content_digest.cpp  # container plan M1: SourceRecord authority rank + content digest (same-size edit, mod order, FST registration order)
    test_pilot_profile_preserve.cpp  # PILOT-PROFILE-PRESERVE-1: editor Profile save decision kernel (offline)
    test_pilot_pick.cpp  # getBestPilot out-of-pool kernel (offline)
    test_ui_defs_lockstep.cpp  # UI-DEFS-LOCKSTEP-HARDEN-1: defs/ui package version + zero-element/zero-draw fail-soft (offline)
    test_hud_tab_registry.cpp  # HUD-IMGUI slice C: tab registry order/cycle invariants (offline)
    test_objectives_panel.cpp
    test_input_capture_policy.cpp  # I3: pure input capture/focus/modal policy + press/release edge kernel (offline)
    test_infowindow_model.cpp  # HUD-IMGUI slice D: info-window component aggregation kernel (offline)
    test_terrain_material.cpp  # TERRAIN-MATERIAL-SINGLE-SOURCE: terrain type -> material table (type 20 = Concrete)
    test_terrain_normal_load.cpp
    test_sim_tick_clock.cpp  # R1: the fixed 30 Hz sim tick accumulator (frame-rate independence, game-speed scaling)
    test_mc2log.cpp         # LOGGING-FACADE-P0: mc2::log facade (level gate, vformat fail-soft, MC2_EVENT bytes, MC2_LOG spec)
    test_mc2fmt.cpp         # STEP3-LOGGING-B0: mc2fmt.h to_buf/to_buf_n/safe/as_* vs snprintf (offline)
    test_printf_corpus.cpp  # STEP3-LOGGING-B0: generated format corpus, printf vs mc2::fmt::to_buf byte equality
    ${MC2_WORKTREE_ROOT}/GameOS/gameos/utils/mc2log.cpp  # LOGGING-FACADE-P0: leaf TU (no engine deps)
    ${MC2_WORKTREE_ROOT}/GameOS/gameos/render_backend_select.cpp  # the MC2_RENDER_BACKEND reporter (backendRequestFromText)
    test_texture_decode.cpp  # backend-neutral texture decode + upload policy (no GL)
    test_ui_chrome_sheets.cpp  # UI-CHROME-SPLIT-1: manifest, sheet re-pack, stored PNG, nine-slice geometry, checked-in manifest
    ${MC2_WORKTREE_ROOT}/mclib/ui_chrome_sheets.cpp
    ${MC2_WORKTREE_ROOT}/GameOS/gameos/texture_decode.cpp
    ${MC2_WORKTREE_ROOT}/GameOS/gameos/utils/Image.cpp     # the TGA / PNG decoders texture_decode wraps (WIC for PNG)
    test_terrain_field_store.cpp  # the CPU terrain fields both backends draw from (no GL)
    ${MC2_WORKTREE_ROOT}/GameOS/gameos/terrain_field_store.cpp
    test_terrain_surface_asset.cpp
    ${MC2_WORKTREE_ROOT}/GameOS/gameos/terrain_surface_asset.cpp
    ${MC2_WORKTREE_ROOT}/GameOS/gameos/cook_cache.cpp
    test_shadow_mask_asset.cpp  # the shadow mask cook: header/payload validation, BC4 block packing, cold/warm/changed/healed cache
    ${MC2_WORKTREE_ROOT}/GameOS/gameos/shadow_mask_asset.cpp
    test_terrain_surface_cook.cpp
    test_terrain_lod_rules.cpp  # stitch stride pairs crack-free, cull z range; header-only (mclib/terrain_lod_rules.h)
    ${MC2_WORKTREE_ROOT}/GameOS/gameos/terrain_surface_cook.cpp
    test_vk_render_pass_timer.cpp  # VK pass timer tick math (counter wrap, valid bits); header-only, no Vulkan
    test_vk_island_fail_policy.cpp  # VK island init failure -> wait/retry/degrade/fatal; header-only, no Vulkan
    test_vk_desc_write_cache.cpp  # descriptor-write cache key (skip, key change, epoch bump, invalidate); header-only, no Vulkan
    test_vk_desc_pool_chain.cpp  # chained descriptor pools past the 192/480/64 caps, injected pool errors, stable chain on reset; header-only, no Vulkan
    test_vk_rt_caps.cpp  # RT tier selection + [VK_RT v1] line from faked caps; header-only, no Vulkan
    test_vk_rt_layout.cpp  # RT AS storage/scratch math, build barrier chain, SBT layout; header-only, no Vulkan
    test_vk_rt_scene.cpp  # RT scene records/keys, prop transform, terrain surface chunks, packet rebase; no Vulkan
    ${MC2_WORKTREE_ROOT}/GameOS/gameos/vk_rt_scene_math.cpp
    test_debug_line_project.cpp  # debug overlay segment projection; header-only
    test_camera_pick_hash.cpp  # pick revision hash: altitude / shift each move it; header-only
    test_hitch_fence.cpp # hitch trace Vulkan fence-wait record; header-only, no Vulkan
    test_hitch_stats.cpp # hitch_stats.h direct fence-wait and staged-byte counters
    test_screenshot_slot.cpp  # screenshot request slot and its expiry; header-only
    test_screen_settle_capture.cpp  # MC2_SHOT_SCREEN settle trigger state machine; header-only
    test_tgl_force_z.cpp  # forceZ reverse-Z mirror, both render paths
    test_static_prop_selflit.cpp  # self-lit two-sided card predicate (stock coil yes, imported override no)
    test_ktx_parse.cpp  # the KTX2 container parser every sidecar texture goes through (bounds on hostile files)
    ${MC2_WORKTREE_ROOT}/RenderCore/KtxLoader.cpp
    ${MC2_WORKTREE_ROOT}/RenderCore/KtxBasis.cpp

    # MINIMAL-VIABLE-HARNESS-1: malloc-backed heap + GameOS-global shim.
    # Replaces heap.cpp (won't compile standalone -- see
    # docs/testing/fitini-inmem-harness-bootstrap-recon-1.md) and
    # gameos.cpp/gameos_debugging.cpp (Environment / InternalFunctionStop).
    # heap.cpp MUST NOT be added below while this shim is linked (ODR).
    heap_shim.cpp

    # Leaf-TU engine sources (no transitive engine include drag).
    ${MC2_WORKTREE_ROOT}/mclib/fst_hash.cpp
    ${MC2_WORKTREE_ROOT}/mclib/appearance_rng.cpp
    ${MC2_WORKTREE_ROOT}/GameOS/gameos/sim_overlap.cpp
    ${MC2_WORKTREE_ROOT}/mclib/unit_animation_tick.cpp
    ${MC2_WORKTREE_ROOT}/mclib/resource_strings.cpp
    ${MC2_WORKTREE_ROOT}/GameOS/gameos/MC2Strings.cpp
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
    ${MC2_WORKTREE_ROOT}/mclib/vfs.cpp        # mod/loose/FST VFS module (was file_mod.inc)
    ${MC2_WORKTREE_ROOT}/mclib/vfs_index.cpp  # mc2::vfs mount table + merged index (split from vfs.cpp)
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
    ${MC2_WORKTREE_ROOT}/code/objective_text.cpp           # objective string reader/writer (leaf: FitIniFile + log facade)
    ${MC2_WORKTREE_ROOT}/code/objective_flags.cpp          # objective boolean flag table (leaf)
    ${MC2_WORKTREE_ROOT}/mclib/mlr/mlr_gate.cpp            # MC2_DISABLE_GOSFX latch

    # MINIMAL-VIABLE-HARNESS-2: paths.cpp is a pure leaf TU (path string globals
    # only -- no mod-index; that logic lives in vfs.cpp, already
    # linked above). Linking it lets tests reference the engine's real path
    # globals (objectPath/artPath/...) instead of stand-ins.
    ${MC2_WORKTREE_ROOT}/mclib/paths.cpp      # path string globals (leaf)

    # V4: the GOS stream handle (gosFileStream) and its gos_* entry points.
    # Disk-only: archive/mod resolution lives above this layer.
    test_gos_file_stream.cpp
    ${MC2_WORKTREE_ROOT}/GameOS/gameos/gameos_fileio.cpp
)
mc2_link_gate(mc2_tests)

# The ABL native-binding headers (ablmc2_internal.h) pull the gui/ and netlib/ headers; only the TUs that include them get those directories.
set_source_files_properties(test_playmovie_deferral.cpp ${MC2_WORKTREE_ROOT}/code/controlgui_chat.cpp ${MC2_WORKTREE_ROOT}/code/mc2movie.cpp PROPERTIES
    INCLUDE_DIRECTORIES "${MC2_WORKTREE_ROOT}/gui;${MC2_WORKTREE_ROOT}/netlib;${MC2_WORKTREE_ROOT}/3rdparty/tracy")
set_source_files_properties(test_abl_position.cpp test_abl_stack_limit.cpp test_cast_census.cpp test_gui_bridge.cpp abl_exec_shim.cpp ${MC2_WORKTREE_ROOT}/code/ablmc2_position.cpp ${MC2_WORKTREE_ROOT}/mclib/abl_script_events.cpp PROPERTIES
    INCLUDE_DIRECTORIES "${MC2_WORKTREE_ROOT}/gui;${MC2_WORKTREE_ROOT}/netlib;${MC2_WORKTREE_ROOT}/3rdparty/tracy")

# LOGGING-FACADE-P0: mc2log.h needs <format>/consteval (C++20); engine headers already build at 23.
set_target_properties(mc2_tests PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)

# The generated printf corpus (one TEST_CASE per converted format pair, thousands of them) exceeds the 65279-section limit of a normal object.
if(MSVC)
    set_source_files_properties(test_printf_corpus.cpp PROPERTIES COMPILE_FLAGS "/bigobj")
endif()

target_include_directories(mc2_tests PRIVATE
    ${MC2_WORKTREE_ROOT}/3rdparty/imgui
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
if(WIN32)
    target_link_libraries(mc2_tests PRIVATE
        ${MC2_WORKTREE_ROOT}/3rdparty/lib/x64/zlib.lib
        ole32 windowscodecs   # utils/Image.cpp decodes PNG through WIC
    )
else()
    find_package(ZLIB REQUIRED)
    target_link_libraries(mc2_tests PRIVATE ZLIB::ZLIB)
    target_sources(mc2_tests PRIVATE
        ${MC2_WORKTREE_ROOT}/GameOS/src/platform_winbase.cpp
        ${MC2_WORKTREE_ROOT}/GameOS/src/platform_winuser.cpp
        ${MC2_WORKTREE_ROOT}/GameOS/src/platform_stdlib.cpp
        ${MC2_WORKTREE_ROOT}/GameOS/src/platform_io.cpp)
endif()
target_sources(mc2_tests PRIVATE test_platform_portability.cpp)

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
    $<$<PLATFORM_ID:Windows>:PLATFORM_WINDOWS>
    USE_ASSEMBLER_CODE=0
)
# MC2_REPO_ROOT / MC2_TEST_FIXTURE_DIR are resolved at runtime by the force-included
# test_repo_root.h: an absolute checkout path in a -D is baked into the object and not
# in the compile-cache key (W114 replayed a deleted worktree's path).
target_include_directories(mc2_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
if(MSVC)
    target_compile_options(mc2_tests PRIVATE /FItest_repo_root.h)
else()
    target_compile_options(mc2_tests PRIVATE SHELL:-include\ test_repo_root.h)
endif()

if(MSVC)
    # /we4002: wrong macro argument count (2-arg CAPTURE misuse) is an error, as it is on clang.
    target_compile_options(mc2_tests PRIVATE /W4 /WX- /we4002)
endif()
mc2_asan_runtime(mc2_tests)

if(NOT WIN32)
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/printf_corpus_gen.inc" _native_corpus)
    string(REPLACE "std::snprintf(" "corpusSnprintf(" _native_corpus "${_native_corpus}")
    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/printf_corpus_native.inc" CONTENT "${_native_corpus}")
    set_source_files_properties(test_printf_corpus.cpp PROPERTIES INCLUDE_DIRECTORIES "${CMAKE_CURRENT_BINARY_DIR}")
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
# exercises ordering independently of the doctest runner.
# Native target ownership is supplied by native_render_state.cpp.
add_executable(test_render_contract_3
    test_render_contract_3.cpp
    ${MC2_WORKTREE_ROOT}/mclib/render_contract.cpp
    ${MC2_WORKTREE_ROOT}/GameOS/gameos/native_render_state.cpp
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

# The former contract executables share doctest.
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
    test_los_kernel.cpp
    test_terrain_daylight_harness.cpp
    test_object_light_row_harness.cpp
    ${MC2_WORKTREE_ROOT}/mclib/render_contract.cpp
    ${MC2_WORKTREE_ROOT}/GameOS/gameos/native_render_state.cpp
    ${MC2_WORKTREE_ROOT}/code/job_system.cpp
    ${MC2_WORKTREE_ROOT}/code/los_kernel.cpp
    ${MC2_WORKTREE_ROOT}/code/jobs_fatal.cpp
)
target_compile_features(mc2_tests PRIVATE cxx_std_23)
target_include_directories(mc2_tests PRIVATE
    ${MC2_WORKTREE_ROOT}/GameOS/gameos/utils
    ${MC2_WORKTREE_ROOT}/3rdparty/include/SDL2
)
find_package(Threads REQUIRED)
if(WIN32)
    list(PREPEND CMAKE_PREFIX_PATH "${MC2_WORKTREE_ROOT}/3rdparty" "${MC2_WORKTREE_ROOT}/3rdparty/cmake")
    list(PREPEND CMAKE_LIBRARY_PATH "${MC2_WORKTREE_ROOT}/3rdparty/lib/x64")
endif()
find_package(SDL2 CONFIG REQUIRED COMPONENTS SDL2)
# KtxBasis.cpp needs the vendored transcoder; this standalone project must build it itself (a bare
# "basisu_transcoder" is only a -l flag here: "ld.lld: unable to find library -lbasisu_transcoder" on the box).
if(NOT TARGET basisu_transcoder)
    enable_language(C)   # zstddeclib.c
    add_subdirectory("${MC2_WORKTREE_ROOT}/3rdparty/basisu" "${CMAKE_BINARY_DIR}/basisu")
endif()
target_link_libraries(mc2_tests PRIVATE
    Threads::Threads
    SDL2::SDL2
    basisu_transcoder
)
if(WIN32)
    # The CPU surface tests use SDL2; no graphics context is created.
    add_custom_command(TARGET mc2_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:SDL2::SDL2>"
            "$<TARGET_FILE_DIR:mc2_tests>"
        COMMENT "Copy SDL2 for CPU surface tests"
        VERBATIM
    )
endif()

# Save schema and ABL output bounds exercise their shared production helpers.
target_sources(mc2_tests PRIVATE
    test_save_archive.cpp
    ${MC2_WORKTREE_ROOT}/code/save_archive.cpp
    test_save_sections.cpp
    test_save_field_omission.cpp
    test_commander_archive.cpp
    test_media_cue_archive.cpp
    test_environment_archive.cpp
    test_contacts_archive.cpp
    test_objective_archive.cpp
    test_logistics_archive.cpp
    test_team_archive.cpp
    test_object_manager_archive.cpp
    test_watch_list_archive.cpp
    test_navigation_archive.cpp
    test_abl_archive.cpp
    test_abl_module_archive.cpp
    test_abl_environment_archive.cpp
    test_warrior_archive.cpp
    test_object_value_codec.cpp
    ${MC2_WORKTREE_ROOT}/code/save_sections.cpp
    test_abl_outarrays.cpp
)
target_sources(mc2_tests PRIVATE test_editor_mission_reader.cpp)
target_sources(mc2_tests PRIVATE test_combat_retirement.cpp)
include("${MC2_WORKTREE_ROOT}/tools/vfx_oracle/CMakeLists.txt")
target_sources(vfx_lifecycle_tests PRIVATE test_vfx_frozen_clouds.cpp)
target_sources(vfx_lifecycle_tests PRIVATE
    test_vfx_native_playback.cpp
    ${MC2_WORKTREE_ROOT}/mclib/fx/fx_runtime.cpp
    ${MC2_WORKTREE_ROOT}/mclib/fx/fx_emitters.cpp
    ${MC2_WORKTREE_ROOT}/mclib/fx/fx_tubes.cpp
    ${MC2_WORKTREE_ROOT}/mclib/fx/fx_geometry.cpp
    ${MC2_WORKTREE_ROOT}/mclib/fx/fx_render.cpp
    ${MC2_WORKTREE_ROOT}/mclib/fx/fx_format.cpp
    ${MC2_WORKTREE_ROOT}/mclib/fx/fx_catalog.cpp
    ${MC2_WORKTREE_ROOT}/mclib/fx/fx_curve.cpp
    ${MC2_WORKTREE_ROOT}/mclib/fx/fx_mesh_data.cpp
    ${MC2_WORKTREE_ROOT}/tools/mc2fx/fx_compare.cpp)
# The frozen-cloud tests own the particle bridge capture instead of the platform sink.
set_source_files_properties("${MC2_WORKTREE_ROOT}/tools/vfx_oracle/lifecycle_platform.cpp"
    PROPERTIES COMPILE_DEFINITIONS "gos_particle_bridge_flush=vfx_test_unused_particle_bridge_flush")
target_sources(mc2_tests PRIVATE test_vfx_frozen_driver.cpp)
target_sources(mc2_tests PRIVATE
    test_sim_rng.cpp test_tactical_order_queue.cpp test_mission_order_ingress.cpp test_authored_route.cpp
    test_command_replay.cpp test_tacorder_wire.cpp test_tactic_table.cpp
    ${MC2_WORKTREE_ROOT}/code/command_replay.cpp
    ${MC2_WORKTREE_ROOT}/code/sim_rng.cpp
    ${MC2_WORKTREE_ROOT}/code/tactical_order_queue.cpp
)
set_source_files_properties(test_mission_order_ingress.cpp PROPERTIES
    INCLUDE_DIRECTORIES "${MC2_WORKTREE_ROOT}/gui;${MC2_WORKTREE_ROOT}/netlib;${MC2_WORKTREE_ROOT}/3rdparty/tracy")
# The Part fixtures compile the same leaf sources as the engine.
target_sources(mc2_tests PRIVATE
    test_part_archive.cpp
    ${MC2_WORKTREE_ROOT}/code/mission_init_parts.cpp
    ${MC2_WORKTREE_ROOT}/code/mission_parts_archive.cpp
    ${MC2_WORKTREE_ROOT}/mclib/cident.cpp
    ${MC2_WORKTREE_ROOT}/mclib/mathfunc.cpp
)
set_source_files_properties(test_part_archive.cpp
    ${MC2_WORKTREE_ROOT}/code/mission_init_parts.cpp
    ${MC2_WORKTREE_ROOT}/code/mission_parts_archive.cpp PROPERTIES
    INCLUDE_DIRECTORIES "${MC2_WORKTREE_ROOT}/gui;${MC2_WORKTREE_ROOT}/netlib;${MC2_WORKTREE_ROOT}/3rdparty/tracy")
# B7o.1: TG_TypeNode virtual destructor (header-only plus local stubs for the base out-of-line virtuals)
target_sources(mc2_tests PRIVATE test_tg_type_node_dtor.cpp)
# B7o.3: actor appearance owner protocol (header-only, code/appearance_owner.h)
target_sources(mc2_tests PRIVATE test_actor_appearance_owner.cpp)
# A2: the ABL debugger managers are created in one place (source pins; the interpreter is not in this tier)
target_sources(mc2_tests PRIVATE test_abl_debugger_lifetime.cpp)

# B7o.4: queued waypoint marker owners (header-only, code/warrior_queue_markers.h): byte-level oracle histories, final pool slice (ASan), rebind, restore
target_sources(mc2_tests PRIVATE test_warrior_queue_markers.cpp)
# B7o.6a/6b: TG_ShapeRec transfer (reset fields), parent rebuild after detach and compaction; since 6b the record owns its node
target_sources(mc2_tests PRIVATE test_tg_shape_rec_transfer.cpp)
# B7 proof gap: GameObject zero-fill against a poisoned operator new (header-only, code/object_zero_new.h)
target_sources(mc2_tests PRIVATE test_object_zero_new.cpp)

# Missing GLM leaves the existing unit tier usable, but cannot prove a math migration.
if(NOT TARGET glm::glm)
    if(EXISTS "${MC2_WORKTREE_ROOT}/3rdparty/glm/glm/glm.hpp")
        add_library(mc2_identity_glm INTERFACE)
        target_include_directories(mc2_identity_glm SYSTEM INTERFACE "${MC2_WORKTREE_ROOT}/3rdparty/glm")
        add_library(glm::glm ALIAS mc2_identity_glm)
    else()
        find_package(glm 1.0.1 EXACT CONFIG QUIET)
    endif()
endif()
if(TARGET glm::glm)
    set(MC2_IDENTITY_SOURCES
        test_glm_math_identity.cpp   # step 5: the glm engine math (convention adapters, byte formats)
    )
    target_sources(mc2_tests PRIVATE ${MC2_IDENTITY_SOURCES})
    target_link_libraries(mc2_tests PRIVATE glm::glm)
    # Uncalled legacy class self-tests reference engine services; only the math is needed here.
    if(MSVC)
        set_property(SOURCE ${MC2_IDENTITY_SOURCES} APPEND PROPERTY COMPILE_OPTIONS /fp:precise /Gy)
        target_link_options(mc2_tests PRIVATE /OPT:REF /INCREMENTAL:NO)
    else()
        set_property(SOURCE ${MC2_IDENTITY_SOURCES} APPEND PROPERTY COMPILE_OPTIONS
            -fno-fast-math -ffp-contract=off -ffunction-sections -fdata-sections)
        if(APPLE)
            target_link_options(mc2_tests PRIVATE -Wl,-dead_strip)
        else()
            target_link_options(mc2_tests PRIVATE -Wl,--gc-sections)
        endif()
    endif()
    message(STATUS "Stuff/GLM bit-identity corpus enabled (GLM 1.0.1, scalar, precise)")
else()
    message(WARNING "Stuff/GLM bit-identity corpus NOT BUILT: vendor GLM 1.0.1; sim migration NOT PROVEN")
endif()

# Preferences use real disk IO; keyboard application remains an engine adapter.
target_sources(mc2_tests PRIVATE
    test_preferences.cpp
    ${MC2_WORKTREE_ROOT}/code/prefs_io.cpp
    ${MC2_WORKTREE_ROOT}/mclib/gamelog.cpp
)
set_source_files_properties(${MC2_WORKTREE_ROOT}/code/prefs_io.cpp test_preferences.cpp PROPERTIES
    INCLUDE_DIRECTORIES "${MC2_WORKTREE_ROOT}/gui;${MC2_WORKTREE_ROOT}/netlib;${MC2_WORKTREE_ROOT}/3rdparty/tracy")

# Exercise the shipped mixer with SDL's dummy device: no game, window, or hardware audio.
find_package(SDL2_mixer REQUIRED)
target_sources(mc2_tests PRIVATE
    test_audio_backend.cpp
    ${MC2_WORKTREE_ROOT}/GameOS/gameos/gameos_sound.cpp
)
set_source_files_properties(${MC2_WORKTREE_ROOT}/GameOS/gameos/gameos_sound.cpp test_audio_backend.cpp PROPERTIES
    INCLUDE_DIRECTORIES "${MC2_WORKTREE_ROOT}/3rdparty/tracy")
target_link_libraries(mc2_tests PRIVATE SDL2_mixer::SDL2_mixer)
if(WIN32)
    add_custom_command(TARGET mc2_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:SDL2_mixer::SDL2_mixer>" "$<TARGET_FILE_DIR:mc2_tests>"
        VERBATIM)
endif()

# Characterize the production query arithmetic without pulling renderer/sim
# services into the unit binary. These are verbatim source slices, regenerated
# on source changes; no second implementation or production query refactor.
function(mc2_height_slice output source begin end)
    file(READ "${MC2_WORKTREE_ROOT}/${source}" text)
    string(FIND "${text}" "${begin}" first)
    string(FIND "${text}" "${end}" last)
    if(first LESS 0 OR last LESS first)
        message(FATAL_ERROR "Terrain height characterization source boundary changed: ${source}")
    endif()
    math(EXPR length "${last} - ${first}")
    string(SUBSTRING "${text}" ${first} ${length} body)
    set(${output} "${body}" PARENT_SCOPE)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${MC2_WORKTREE_ROOT}/${source}")
endfunction()
# Trigger paths: code/controlgui_init.cpp and tests/unit/test_hud_frame.cpp.
mc2_height_slice(hud_frame_bake code/controlgui_init.cpp
    "void ControlGui::bakeFrame()" "void ControlGui::initStatics(")
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/hud_frame_bake.inc" CONTENT "${hud_frame_bake}")
target_sources(mc2_tests PRIVATE test_hud_frame.cpp)
set_source_files_properties(test_hud_frame.cpp PROPERTIES INCLUDE_DIRECTORIES "${CMAKE_CURRENT_BINARY_DIR}")
mc2_height_slice(objectives_canvas GameOS/gameos/gameos_graphics_params.cpp
    "static bool computeUiCanvas16x9(" "bool __stdcall gos_ComputeUiCanvasBox(")
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/objectives_canvas.inc" CONTENT "${objectives_canvas}")
mc2_height_slice(objectives_batch GameOS/gameos/gameos_graphics_hud.cpp
    "void gosRenderer::endHudPanel(" "static int get_next_break(")
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/objectives_batch.inc" CONTENT "${objectives_batch}")
set_source_files_properties(test_objectives_panel.cpp PROPERTIES INCLUDE_DIRECTORIES "${CMAKE_CURRENT_BINARY_DIR}")
mc2_height_slice(camera_world_lights mclib/camera.h
    "long addWorldLight (" "glm::vec3 getScreenRes (")
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/camera_world_lights.inc" CONTENT "${camera_world_lights}")
mc2_height_slice(camera_release_object code/simplecamera.cpp
    "void SimpleCamera::releaseObject()" "SimpleCamera::~SimpleCamera()")
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/camera_release_object.inc" CONTENT "${camera_release_object}")
target_sources(mc2_tests PRIVATE test_world_light_tags.cpp ${MC2_WORKTREE_ROOT}/mclib/spotlight_diag.cpp)
set_source_files_properties(test_world_light_tags.cpp PROPERTIES INCLUDE_DIRECTORIES "${CMAKE_CURRENT_BINARY_DIR}")
mc2_height_slice(overlay_static_upload GameOS/gameos/vulkan_terrain_island_overlay.cpp
    "bool ovl_prepare_static(" "bool ovl_init(")
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/overlay_static_upload.inc" CONTENT "${overlay_static_upload}")
mc2_height_slice(hud_warm_variants GameOS/gameos/engine_present_vk.cpp
    "struct WarmVariant {" "for (const auto& variant : variants)")
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/hud_warm_variants.inc" CONTENT "${hud_warm_variants}")
mc2_height_slice(overlay_road_inputs GameOS/gameos/gos_terrain_indirect_cement.cpp
    "static bool TileIsRoadArt(" "void PopulateRecipeCementWords()")
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/overlay_road_inputs.inc" CONTENT "${overlay_road_inputs}")
target_sources(mc2_tests PRIVATE test_overlay_prestage.cpp)
set_source_files_properties(test_overlay_prestage.cpp PROPERTIES INCLUDE_DIRECTORIES "${CMAKE_CURRENT_BINARY_DIR}")
mc2_height_slice(light_lists_stage GameOS/gameos/vk_light_lists.h
    "    bool stage(" "    // The frame's one dispatch, split")
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/light_lists_stage.inc" CONTENT "${light_lists_stage}")
mc2_height_slice(terrain_light_stage GameOS/gameos/vulkan_terrain_island_run.cpp
    "bool stage_light_pool(" "// Fill one TerrainFrameUBO")
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/terrain_light_stage.inc" CONTENT "${terrain_light_stage}")
target_sources(mc2_tests PRIVATE test_light_list_recovery.cpp)
set_source_files_properties(test_light_list_recovery.cpp PROPERTIES INCLUDE_DIRECTORIES "${CMAKE_CURRENT_BINARY_DIR}")
if(TARGET glm::glm)
    mc2_height_slice(height_index mclib/mapdata.cpp
        "float MapData::terrainElevation( long indexY, long indexX )" "void MapData::setTerrain(")
    mc2_height_slice(height_world mclib/mapdata.cpp
        "static void cpu_rgb2hsv(" "void MapData::unselectAll()")
    mc2_height_slice(height_valid mclib/terrain_raycast.cpp
        "bool Terrain::IsValidTerrainPosition (" "bool Terrain::IsEditorSelectTerrainPosition (")
    mc2_height_slice(height_runtime mclib/terrain_runtime.cpp
        "float sampleGameplayHeight(" "int sampleMaterialId(")
    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/terrain_height_queries.inc"
        CONTENT "${height_index}\n${height_world}")
    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/terrain_height_valid.inc"
        CONTENT "${height_valid}")
    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/terrain_height_runtime.inc"
        CONTENT "${height_runtime}")
    target_sources(mc2_tests PRIVATE test_terrain_height_compat.cpp)
    set_source_files_properties(test_terrain_height_compat.cpp PROPERTIES
        INCLUDE_DIRECTORIES "${CMAKE_CURRENT_BINARY_DIR}"
        COMPILE_OPTIONS "$<$<BOOL:${MSVC}>:/fp:precise>")
endif()

target_sources(mc2_tests PRIVATE test_tgl_registry.cpp)
target_sources(mc2_tests PRIVATE test_content_error.cpp ${MC2_WORKTREE_ROOT}/mclib/content_error.cpp)

# Repair deployment: use the production slot acquisition and sensor removal,
# with only the engine services replaced by device-free seams.
mc2_height_slice(repair_vehicle_acquire code/objmgr.cpp
    "GroundVehiclePtr GameObjectManager::newVehicle (void)" "void GameObjectManager::setWatchID (")
mc2_height_slice(repair_sensor_remove code/contact.cpp
    "void TeamSensorSystem::removeSensor (SensorSystemPtr sensor)" "void TeamSensorSystem::update (void)")
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/repair_vehicle_acquire.inc" CONTENT "${repair_vehicle_acquire}")
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/repair_sensor_remove.inc" CONTENT "${repair_sensor_remove}")
target_sources(mc2_tests PRIVATE test_repair_deploy.cpp)
set_source_files_properties(test_repair_deploy.cpp PROPERTIES INCLUDE_DIRECTORIES "${CMAKE_CURRENT_BINARY_DIR}")
mc2_height_slice(radio_playback code/gamesound.cpp
    "void GameSoundSystem::playNoiseClip (void)" "// End of radio playback")
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/radio_playback.inc" CONTENT "${radio_playback}")
target_sources(mc2_tests PRIVATE test_radio_playback.cpp)
set_source_files_properties(test_radio_playback.cpp PROPERTIES INCLUDE_DIRECTORIES "${CMAKE_CURRENT_BINARY_DIR}")
mc2_height_slice(tree_dynamic_default mclib/bdactor_internal.h
    "template <mc2::gate::Name n>" "inline constexpr float SPINRATE")
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/tree_dynamic_default.inc" CONTENT "${tree_dynamic_default}")
mc2_height_slice(tree_recovery_gate mclib/bdactor_render.cpp
    "static const bool s_forceDynamicTrees =" "if (s_forceDynamicTrees)")
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/tree_recovery_gate.inc" CONTENT "${tree_recovery_gate}")
target_sources(mc2_tests PRIVATE test_tree_bake_recovery.cpp)
set_source_files_properties(test_tree_bake_recovery.cpp PROPERTIES INCLUDE_DIRECTORIES "${CMAKE_CURRENT_BINARY_DIR}")
mc2_height_slice(terrain_anim_access mclib/appear.h "    mc2::sim::UnitAnimState& unitAnim(" "    auto simulationAnimation()")
mc2_height_slice(terrain_tick mclib/bdactor_update.cpp "    if (!simPresentationPass || !simulationOwned()) {" "        if (simulationOwned() || mc2::overlap::onSimWorker())")
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/terrain_anim_access.inc" CONTENT "${terrain_anim_access}")
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/terrain_tick.inc" CONTENT "${terrain_tick}\n} return TRUE;")
set_source_files_properties(test_terrain_dirty_queue.cpp PROPERTIES INCLUDE_DIRECTORIES "${CMAKE_CURRENT_BINARY_DIR}")
