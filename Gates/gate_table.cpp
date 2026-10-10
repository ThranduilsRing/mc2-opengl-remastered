// Gates/gate_table.cpp -- THE gate table: one row per MC2_* environment variable the engine
// reads. The compiler is its parser (tools/gate_dump emits Gates/gate_table.json from the
// compiled rows); nothing parses this file with a regex.
//
// Row = G(name, Kind, default, owner, retireBy, doc[, options])    Std rule
//       L(Rule, name, Kind, default, owner, retireBy, doc[, options])   legacy rule, kept so a
//                                                                  migrated site reads exactly as before
//       P(...)                                                      numeric row with atoi/atof parsing
// Rows stay one per line, sorted by name, names unique, doc at most 160 characters; long prose
// lives in docs/gate-history.md (see the `// history:` pointers). Adding or renaming a row
// needs `py -3 tools/gate_gen.py --write` so the index follows.
#include "gate_table_dsl.h"

#ifndef GATE_INDEX_BOOTSTRAP
#include "gate_index.gen.h"
#endif

// The table lives in rows::tbl so that the kind names (Bool, Tri, ...) found through
// `using enum Kind` in rows win over the type mc2::gate::Tri.
namespace mc2::gate::rows::tbl {

constexpr Row kTable[] = {
L(Presence, "MC2_3DVIEW_DIAG", Bool, false, "gameos", "", ""),
L(Lead1, "MC2_ABL_ARG_GUARD", Bool, false, "code", "", "ABL crash-safety gates (ABL-OPCODE-BOUNDS-HARDEN) MC2_ABL_ARG_GUARD=1 - enables dispatch-bounds soft path + out-ptr null guards MC2_ABL_RUNTIME_SOFTFAIL=1 -...", passthrough),
L(Lead1, "MC2_ABL_ARG_GUARD_REPRO", Bool, false, "code", "", "", passthrough),
L(Lead0Kill, "MC2_ABL_COMPILE_FATAL", Bool, true, "code", "", "ASAN-3-ABL-COMPILE-FATAL-1: fail at preprocess when ABL syntax errors leave partial bytecode; set =0 only for legacy execute-anyway diagnostics.", passthrough),
L(Presence, "MC2_ABL_COMPILE_FATAL_REPRO", Bool, false, "code", "", "ASAN-3-ABL-COMPILE-FATAL-1: preprocess the supplied malformed-fixture path during ABL init to prove fail-loud behavior.", passthrough),
L(AtoiNZ, "MC2_ABL_CORE_TRACE", Bool, false, "code", "", ""),
L(Presence, "MC2_ABL_REG_TRACE", Bool, false, "unclassified", "", ""),
L(Lead1, "MC2_ABL_RUNTIME_SOFTFAIL", Bool, false, "code", "", "", passthrough),
L(Presence, "MC2_ABL_SKIP_ERRORED_MODULES", Bool, false, "code", "", "", passthrough),
L(Presence, "MC2_ABL_TRACE", Bool, false, "code", "", ""),
L(Lead0, "MC2_ABL_VM_FACTS", Bool, false, "code", "", ""),
G("MC2_ACTIVE_MOD", String, "", "code", "", "", reread | passthrough),
// history: docs/gate-history.md#MC2_AIMED_FIRE_FIXTURE
L(Lead0P, "MC2_AIMED_FIRE_FIXTURE", Bool, false, "code", "", "ISSUE-9 headless probe (code/warrior.cpp MechWarrior::updateActions): every home-team mech without a combat order is issued an aimed-HEAD attack...", passthrough),
L(Presence, "MC2_ALPHA_TEST_TRACE", Bool, false, "props", "", ""),
L(Presence, "MC2_AMBIENT_ASSERT_FATAL", Bool, false, "code", "", ""),
// history: docs/gate-history.md#MC2_ANIM_CADENCE_FIX
L(Lead0Kill, "MC2_ANIM_CADENCE_FIX", Bool, true, "mech", "", "ANIM-CADENCE-FIX: advance mech gait at most once per render frame; prevents a visible double-step when Mover::getLOSPosition() re-invokes...", passthrough),
L(Presence, "MC2_ANIM_CADENCE_GUARD", Bool, false, "mech", "", "", passthrough),
// history: docs/gate-history.md#MC2_APPEAR_AUDIT
L(Lead0, "MC2_APPEAR_AUDIT", Bool, false, "code", "", "META-FIX design B PART 4 (code/mission.cpp Mission::start beside mission_phase_mark('mission_ready')): default OFF, OBSERVATIONAL-FIRST.", passthrough),
// history: docs/gate-history.md#MC2_ASSET_FAILLOUD
L(Lead0Kill, "MC2_ASSET_FAILLOUD", Bool, true, "code", "", "META-FIX design B PART 1 (mclib/tgl_load_guard.cpp): default-ON killswitch (MC2_TERRAIN_TESS shape - unset=ON, =0 exact legacy fail-soft).", passthrough),
L(Lead1, "MC2_ASSET_SCALE_SELFTEST", Bool, false, "gameos", "", ""),
L(Lead1, "MC2_ASSET_SCALE_TRACE", Bool, false, "gameos", "", ""),
// history: docs/gate-history.md#MC2_ASSET_TRACE
L(Lead0, "MC2_ASSET_TRACE", Bool, false, "code", "", "META-FIX design B PART 0 companion (mclib/msl.cpp loader chokepoint): =1 additionally logs '[ASSET_LOAD v1] event=file_missing ...' for MISSING files at the...", passthrough),
L(Lead0Kill, "MC2_ASSIMP_IMPORT", Bool, true, "code", "", ""),
// history: docs/gate-history.md#MC2_ASSIMP_MECH_IMPORT
L(Presence, "MC2_ASSIMP_MECH_IMPORT", Bool, false, "mech", "", "BT2018 mech import 1A: route mech geometry through the Assimp GLTF/FBX importer instead of the legacy MSL loader."),
L(Lead0Kill, "MC2_ASSIMP_PAUSE_GATE", Bool, true, "mech", "", ""),
// history: docs/gate-history.md#MC2_ASSIMP_TRACE
L(Presence, "MC2_ASSIMP_TRACE", Bool, false, "code", "", "TRACKV-CI-GATE-RESTORE-1: verbose Assimp loader trace."),
L(Presence, "MC2_BATCHER_FLUSH_TIMING", Bool, false, "props", "", ""),
L(Presence, "MC2_BLDG_DIAG_TRACE", Bool, false, "props", "", ""),
L(Presence, "MC2_BLDG_REG_TRACE", Bool, false, "props", "", ""),
L(Lead0Kill, "MC2_BLDG_TYPE_ANIM_STATIC_ELIGIBLE", Bool, true, "props", "", "", passthrough),
L(Presence, "MC2_BLKIDX_TRACE", Bool, false, "props", "", ""),
L(Presence, "MC2_BLOCK_FRUSTUM_FALLBACK", Bool, false, "terrain", "", "F6 T1/T2: cache frustum planes once per frame (O(1)) into Camera member."),
L(NonEmpty, "MC2_BOOKMARK_SAVE_DIR", Bool, false, "code", "", ""),
G("MC2_BOOKMARK_SAVE_TEST", Bool, false, "code", "", ""),  // default not derivable from the tree
// history: docs/gate-history.md#MC2_BOOT_INFO
P("MC2_BOOT_INFO", Int, 0, "code", "", "INFO-CAPTURE fixture: =N force-selects the first home mover into the in-mission Info window and switches the command bar to the INFO tab ONCE at frame >= N..."),
// history: docs/gate-history.md#MC2_BOOT_INFO_SCROLL
L(Lead0, "MC2_BOOT_INFO_SCROLL", Bool, false, "code", "", "INFO-IMGUI-SLICE-4 capture assist: =1 (with MC2_BOOT_INFO=N driving) pins the info-panel loadout list scrolled to bottom from frame N+3000 on BOTH paths -..."),
// history: docs/gate-history.md#MC2_BOOT_OBJ
P("MC2_BOOT_OBJ", Int, 0, "code", "", "OBJ-CAPTURE fixture: =N calls ControlGui::startObjectives(1) ONCE at frame >= N (ControlGui::update), showing the in-mission objectives overlay."),
// history: docs/gate-history.md#MC2_BOOT_OBJ_COMPLETE
P("MC2_BOOT_OBJ_COMPLETE", Int, 0, "code", "", "OBJ-COMPLETE-CAPTURE fixture: =N force-resolves the first active non-hidden objective to OS_SUCCESSFUL ONCE at frame >= N (ControlGui::update) via the..."),
// history: docs/gate-history.md#MC2_BOOT_PAUSED
P("MC2_BOOT_PAUSED", Int, 0, "code", "", "PAUSE-IMGUI-SLICE-0: =N auto-toggles the in-mission pause menu ONCE at frame >= N (MissionInterfaceManager::update)."),
// history: docs/gate-history.md#MC2_BOOT_PAUSED_CONFIRM
L(NonEmpty, "MC2_BOOT_PAUSED_CONFIRM", Bool, false, "code", "", "PAUSE-IMGUI-SLICE-3: =abort|exit invokes the ABORT MISSION / EXIT GAME confirm dialog ONCE after the MC2_BOOT_PAUSED pause menu has settled..."),
P("MC2_BOOT_QUICKLOAD_AT", Int, 0, "code", "", ""),
P("MC2_BOOT_QUICKSAVE_AT", Int, 0, "code", "", ""),
// history: docs/gate-history.md#MC2_BOOT_TIMER
P("MC2_BOOT_TIMER", Int, 0, "code", "", "TIMER-CAPTURE fixture: =N force-arms the in-mission mission timer ONCE at frame >= N by setting mission->m_timeLimit = scenarioTime + 754 s..."),
// history: docs/gate-history.md#MC2_BOOT_TIMER_SECS
P("MC2_BOOT_TIMER_SECS", Float, 754.0, "code", "", "TIMER-CAPTURE fixture parameter: =S overrides the remaining seconds the MC2_BOOT_TIMER fixture arms (default 754)."),
L(NonEmpty, "MC2_BOOT_TO_BAY", Bool, false, "code", "", "Headless harness: mission-objective briefing videos (CPlayBIK) reach here, bypassing the MC2_BOOT_TO_BAY gate in Logistics::playFullScreenVideo."),
L(Presence, "MC2_BOOT_TO_MECHLAB", Bool, false, "mech", "", ""),
L(NonEmpty, "MC2_BOOT_TO_MISSION", Bool, false, "code", "", "MC2_BOOT_TO_MISSION=<mission FileName>: jump to a specific mission's logistics instead of the campaign's first mission."),
L(NonEmpty, "MC2_BOOT_TO_SCREEN", Bool, false, "mech", "", ""),
// history: docs/gate-history.md#MC2_BOOT_VIDEO
L(Lead0, "MC2_BOOT_VIDEO", Bool, false, "code", "", "VIDEO-CAPTURE fixture: =1 overrides the smoke-mode movie skip in MC2Movie::init so in-mission comm videos (mc2_01 V1A.bik ~frame 1600) play under..."),
L(Lead0, "MC2_BOX_DECAL", Bool, false, "post", "", ""),
P("MC2_BOX_DECAL_NORMAL_REJECT", Float, 0.0, "post", "", ""),  // default not derivable from the tree
P("MC2_BOX_DECAL_STRENGTH", Float, 0.0, "post", "", ""),  // default not derivable from the tree
P("MC2_BOX_DECAL_YSPAN", Float, 0.0, "post", "", ""),  // default not derivable from the tree
L(AtoiNZ, "MC2_BRAIN_ALIAS", Bool, false, "code", "", ""),
L(AtoiNZ, "MC2_BRAIN_COMMIT_PHASE", Bool, false, "code", "", ""),
// history: docs/gate-history.md#MC2_BRAIN_DISPATCH
L(AtoiNZ, "MC2_BRAIN_DISPATCH", Bool, false, "code", "", "TechScript special-dispatch master gate (mission.cpp/brain_special_dispatch.cpp).", passthrough),
G("MC2_BRAIN_DISPATCH_APPLY", Bool, false, "code", "", "", passthrough),
G("MC2_BRAIN_DISPATCH_CALL", Bool, false, "code", "", "", passthrough),
L(AtoiNZ, "MC2_BRAIN_DISPATCH_FSM_TODO", Bool, false, "code", "", "", passthrough),
L(AtoiNZ, "MC2_BRAIN_DISPATCH_VAR", Bool, false, "code", "", "", passthrough),
L(AtoiNZ, "MC2_BRAIN_ENGAGE", Bool, false, "code", "", "", reread),
L(AtoiNZ, "MC2_BRAIN_FIXED_TICK", Bool, false, "code", "", ""),
P("MC2_BRAIN_FIXED_TICK_HZ", Float, 10.0, "code", "", "", range(1.0, 30.0)),
L(AtoiNZ, "MC2_BRAIN_FIXED_TICK_TRACE", Bool, false, "code", "", ""),
L(AtoiNZ, "MC2_BRAIN_FLOW", Bool, false, "code", "", ""),
// history: docs/gate-history.md#MC2_BRAIN_FREEZE_TRACE
L(Lead0, "MC2_BRAIN_FREEZE_TRACE", Bool, false, "code", "", "SAVE/LOAD-AIFREEZE-TRACE-1 (+ ISSUE-10 extension): one-line stderr trace at end of MechWarrior::runBrain (brainState, curTacOrder.code, movePath.numSteps...", passthrough),
L(AtoiNZ, "MC2_BRAIN_FSM", Bool, false, "code", "", ""),
L(AtoiNZ, "MC2_BRAIN_INLINE_EMPTY_SKIP", Bool, false, "code", "", "FULL-CAMPAIGN-1: TechScript/Enhanced-converted missions (carver_v_enhanced) leave ScenarioScript empty - the per-warrior brains move to inline Brain{}..."),
L(AtoiNZ, "MC2_BRAIN_INTENT_QUEUE", Bool, false, "code", "", ""),
L(AtoiNZ, "MC2_BRAIN_MISSIONFIT_OPORD", Bool, false, "code", "", "BRAIN-MISSIONFIT-OPORD-CONSUMER-1: drive declarative patrol from the inline Brain{} PrimaryOPORD blocks in mission.fit (carver_v_enhanced format)."),
P("MC2_BRAIN_MISSION_SEED", Int, 0, "code", "", ""),  // default not derivable from the tree
L(AtoiNZ, "MC2_BRAIN_PATROL", Bool, false, "code", "", "", reread),
// history: docs/gate-history.md#MC2_BRAIN_RUNTIME
L(AtoiNZ, "MC2_BRAIN_RUNTIME", Bool, false, "code", "", "TechBrain declarative-brain runtime master gate (warrior.cpp).", passthrough),
// history: docs/gate-history.md#MC2_BRAIN_RUNTIME_APPLY
L(AtoiNZ, "MC2_BRAIN_RUNTIME_APPLY", Bool, false, "code", "", "TechBrain runtime apply gate (warrior.cpp).", passthrough),
G("MC2_BRAIN_RUNTIME_FORCE_MODE", Enum, "", "code", "", "MC2_BRAIN_RUNTIME_FORCE_MODE overrides all per-unit modes (post-load global override).", passthrough | tokens("|enhanced|hybrid")),
L(AtoiNZ, "MC2_BRAIN_RUNTIME_TRACE", Bool, false, "code", "", "", passthrough),
L(AtoiNZ, "MC2_BRAIN_SCOPE_GLOBAL", Bool, false, "code", "", ""),
L(AtoiNZ, "MC2_BRAIN_SNAPSHOT", Bool, false, "code", "", ""),
G("MC2_BRAIN_SPECIAL_FIT", Bool, false, "code", "", "DISPATCH-EFFECT-UNITEJECT-1: MC2_BRAIN_SPECIAL_FIT allows test-fixture override.", passthrough),  // default not derivable from the tree
L(AtoiNZ, "MC2_BRAIN_TASKQ", Bool, false, "code", "", "", passthrough),
L(AtoiNZ, "MC2_BRAIN_TASKQ_TRACE", Bool, false, "code", "", "", passthrough),
L(AtoiNZ, "MC2_BRAIN_VARIANTOF", Bool, false, "code", "", ""),
L(AtoiNZ, "MC2_BRAIN_VAR_MISSION", Bool, false, "code", "", ""),
P("MC2_BRIDGE_MOVER_PERIOD_SEC", Float, 2.0, "code", "", "", passthrough),
L(Presence, "MC2_BRIDGE_MOVER_STATE", Bool, false, "unclassified", "", "", passthrough),
L(Presence, "MC2_BUCKET_CENSUS", Bool, false, "terrain", "", ""),
L(Presence, "MC2_BUCKET_HEADER_TRACE", Bool, false, "terrain", "", "Step 2b: bucket-header SSBO demoted behind MC2_BUCKET_HEADER_TRACE."),
// history: docs/gate-history.md#MC2_BUCKET_ORDER_TRACE
L(Presence, "MC2_BUCKET_ORDER_TRACE", Bool, false, "props", "", "STATICPROP-PERSISTENT-STATIC-BUCKETS-2B-STAGE2 Task-0 probe: per-type static-vs-dynamic add counts + order ([BUCKET_ORDER v1]).", passthrough),
L(Lead0Kill, "MC2_BUILDING_FOOTPRINT_SHADOW", Bool, true, "props", "", ""),
L(Exact1, "MC2_BUILDING_PBR", Bool, false, "props", "", ""),
L(Presence, "MC2_BUILDING_PBR_TRACE", Bool, false, "code", "", ""),
// history: docs/gate-history.md#MC2_BURNIN_NO_JPG
G("MC2_BURNIN_NO_JPG", Bool, false, "terrain", "", "BURNIN-DISK-JPEG kill-switch: when set, skip the <name>.burnin.jpg sidecar probe in TerrainColorMap::init and load the full <name>.burnin.tga (or .tga) instead."),
L(Lead0P, "MC2_CALLED_SHOT_FIX", Bool, false, "mech", "", "ISSUE-9 called-shot effectiveness (gameplay gate, default OFF = stock): (S1) Mover::calcAttackChance floors the aimed-shot modifiers at Head 0.30 / Arm 0.45...", passthrough),
L(Lead0Kill, "MC2_CAMERA_ASPECT_NATIVE", Bool, true, "code", "", "", passthrough),
// history: docs/gate-history.md#MC2_CAMERA_MOVE_DIAG
L(Presence, "MC2_CAMERA_MOVE_DIAG", Bool, false, "unclassified", "", "TRACKV-CI-GATE-RESTORE-1: camera movement diagnostic readback probe.", passthrough),
// history: docs/gate-history.md#MC2_CAMERA_PARK
L(Presence, "MC2_CAMERA_PARK", Bool, false, "gameos", "", "CAMERA-PARK-1 (ported from vk branch 4b69ef78): ='x,y,rot,proj,alt[@frame]' hard-sets the live camera pose once at frame >= @frame (default 300) and pins it...", passthrough),
P("MC2_CAMERA_SMOOTH", Float, 100.0, "code", "", "", passthrough | range(0.0, 2000.0)),
L(Lead1, "MC2_CAMERA_SMOOTH_TRACE", Bool, false, "code", "", "", passthrough),
L(Lead1, "MC2_CAM_SMOOTH_TEST", Bool, false, "code", "", "", passthrough),
// history: docs/gate-history.md#MC2_CEMENT_ALPHA_ATLAS
L(Lead0Kill, "MC2_CEMENT_ALPHA_ATLAS", Bool, true, "terrain", "", "DEFAULT ON since RUNWAY-CLOSE-1 (2026-08-02), measured at the user's mc2_10 saved_204907 bookmark, exe 30c18df1: rescued swath pixels 15 -> 6506, rescued...", passthrough),
// history: docs/gate-history.md#MC2_CEMENT_ATLAS_DUMP
L(NonEmpty, "MC2_CEMENT_ATLAS_DUMP", Bool, false, "terrain", "", "CEMENT-ATLAS-CELL-2: dump the assembled cement/road overlay atlas so cell art can be inspected directly -- which cells actually carry painted markings is an...", passthrough),
// history: docs/gate-history.md#MC2_CEMENT_ATLAS_MAGENTA_KEY
L(Lead0, "MC2_CEMENT_ATLAS_MAGENTA_KEY", Bool, false, "terrain", "", "CEMENT-ATLAS-MAGENTA-KEY-1 (MC2-03-CEMENT-PINK-1): solid cement catalog cells reach the atlas via TerrainTextures::textureFromMemory (gos_Texture_Solid)...", passthrough),
L(Lead0Kill, "MC2_CEMENT_ATLAS_SEAM_FIX", Bool, true, "terrain", "", ""),
// history: docs/gate-history.md#MC2_CEMENT_CONCRETE_CANON
L(Lead0, "MC2_CEMENT_CONCRETE_CANON", Bool, false, "terrain", "", "CEMENT-ATLAS-CELL-2 ARM B: point every plain-concrete tile at ONE canonical concrete cell, so an apron cannot change colour tile to tile at all.", passthrough),
// history: docs/gate-history.md#MC2_CEMENT_CONCRETE_TONE
L(Lead0, "MC2_CEMENT_CONCRETE_TONE", Bool, false, "terrain", "", "CEMENT-ATLAS-CELL-2 ARM A: tone-normalise ONLY the actual-concrete atlas cells.", passthrough),
// history: docs/gate-history.md#MC2_CEMENT_LIT
P("MC2_CEMENT_LIT", Float, 0.0, "terrain", "", "RUNWAY-APRON-TONE-2 (user report, mc2_10 @ bookmark 204937): 0..1 share of the terrain lighting term that CEMENT fragments obey; default 0.0 == stock fully...", passthrough | range(0.0, 1.0)),
L(Lead0Kill, "MC2_CEMENT_MISSION_LIGHT", Bool, true, "gameos", "", ""),
// history: docs/gate-history.md#MC2_CEMENT_RAMP_WORD
L(Lead0, "MC2_CEMENT_RAMP_WORD", Bool, false, "terrain", "", "CEMENT-WORD-RAMP-1: the ALBEDO-side cure for the project's longest-running user complaint, the dark outlines around every concrete pad and taxiway...", passthrough),
// history: docs/gate-history.md#MC2_CEMENT_ROAD_ART_DETECT
L(Lead0, "MC2_CEMENT_ROAD_ART_DETECT", Bool, false, "terrain", "", "CEMENT-INTRACELL-1 ARM C: detect the BAKED ASPHALT ROAD TILESET authored inside SOLID cement catalog cells, and exclude those cells from every concrete...", passthrough),
// history: docs/gate-history.md#MC2_CEMENT_TRANSITION_FALLBACK
L(Lead0, "MC2_CEMENT_TRANSITION_FALLBACK", Bool, false, "terrain", "", "CEMENT-TRANSITION-FALLBACK-1 (MC2-03-CEMENT-PINK-1): cure MISSING ROAD SWATHS (user report, mc2_24).", passthrough),
L(Presence, "MC2_CHEAT_INFINITE_MONEY", Bool, false, "mech", "", "Interactive cheat gates -- read once at startup, default OFF (unset = nullptr = false)"),
L(Presence, "MC2_CHEAT_SALVAGE_ALL", Bool, false, "mech", "", ""),
L(Lead0, "MC2_CLOUD_SHADOW", Bool, false, "post", "", "", passthrough),
L(Lead0P, "MC2_CLUSTER_DEPTH_PYRAMID", Bool, false, "gameos", "", ""),
L(Lead0P, "MC2_CLUSTER_DEPTH_PYRAMID_PLANT", Bool, false, "gameos", "", ""),
L(Lead0P, "MC2_CLUSTER_DEPTH_PYRAMID_VERIFY", Bool, false, "gameos", "", ""),
G("MC2_COALESCE_FORCE_DISARM", Enum, "", "props", "", "Step 12B.1 - exact, case-sensitive string match on the four valid values; anything else (including unset) leaves None.", tokens("|alloc_failed|mixed_alpha|no_extension|size_mismatch")),
L(Presence, "MC2_COALESCE_GPU_VS_CPU_COUNT_TRACE", Bool, false, "props", "", "Env probe for GPU-vs-CPU count diagnostic (Step 4.11)."),
L(Lead0, "MC2_COLLISION_FACTS", Bool, false, "code", "", ""),
// history: docs/gate-history.md#MC2_COLORMAP_CPU_RETIRE
L(Lead0Kill, "MC2_COLORMAP_CPU_RETIRE", Bool, true, "terrain", "", "COLORMAP-CPU-RETIRE-1: free cpuColorMap + cpuDispAlpha in BuildColormapAtlas immediately after GPU atlas upload."),
// history: docs/gate-history.md#MC2_COLORMAP_DISPLACE_PROBE
L(Presence, "MC2_COLORMAP_DISPLACE_PROBE", Bool, false, "code", "", "COLORMAP-CPU-RETIRE-1 parity probe: logs displacement magnitude (maxDispMag in wu) from the CPU colormap displacement path."),
// history: docs/gate-history.md#MC2_COLORMAP_KTX2
L(Lead0Kill, "MC2_COLORMAP_KTX2", Bool, true, "terrain", "", "COLORMAP-BC7-KTX2-1: prefer .burnin.ktx2 BC7 sidecar in BuildColormapAtlas (gos_terrain_indirect.cpp)."),
L(Presence, "MC2_CONE_LEAF_DUMP", Bool, false, "props", "", "CONE-LEAF-NAME-DIAG (batch-8C, MC2_CONE_LEAF_DUMP, default OFF): dump every static-prop recipe leaf's real node name + flags."),
// history: docs/gate-history.md#MC2_CONE_POOL_POLICY
L(Lead0Kill, "MC2_CONE_POOL_POLICY", Bool, true, "code", "", "CONE-SPILL-POLICY-1: pool-placement policy for building/beacon spotlight ground pools on the DEFAULT path (MC2_CONE_SPILL_COUPLING OFF).", passthrough),
// history: docs/gate-history.md#MC2_CONE_POOL_REACH
P("MC2_CONE_POOL_REACH", Float, 0.0, "code", "", "CONE-SPILL-POLICY-1: reach multiplier k for the extended beam/terrain intersection search (reach = k * authored coneLen * MC2_LIGHT_CONE_SPILL).", passthrough | clampRange(1.0, 16.0)),  // default not derivable from the tree
// history: docs/gate-history.md#MC2_CONE_SPILL_COUPLING
L(Lead1, "MC2_CONE_SPILL_COUPLING", Bool, false, "code", "", "CONE-SPILL-GRADE-1: isolates CONE-SPILL-COUPLING-1 (c1f98b45), which shipped UNGATED as a default-behaviour change riding MC2_LIGHT_CONE_NODES and is now...", passthrough),
G("MC2_CONFIG_DUMP", Enum, "default", "gameos", "", "[CONFIG v1] dump scope: default (env-set gates), all (every row), none (summary only)", tokens("default|all|none")),
G("MC2_CONFIG_STRICT", Bool, false, "gameos", "", "make a bad value or an unregistered MC2_* variable fatal at startup"),
L(Presence, "MC2_CONTRACT_REPO_ROOT", Bool, false, "unclassified", "", ""),
// history: docs/gate-history.md#MC2_CPU_PHONG_RETIRE
L(Lead0Kill, "MC2_CPU_PHONG_RETIRE", Bool, true, "code", "", "[DEFAULT-ON KILLSWITCH; '=0' is the only revert token -- MC2_TERRAIN_TESS / MC2_COLORMAP_CPU_RETIRE shape.]", passthrough),
L(Lead1, "MC2_CPU_PROJ_COST_SPLIT", Bool, false, "code", "", "", passthrough),
L(Presence, "MC2_CRASH_FAST_EXIT", Bool, false, "gameos", "", "Headless crash fast-exit: write the bundle, then terminate WITHOUT the modal dialog so an automated soak/harness process exits immediately on crash (the..."),
G("MC2_CURSOR_EDGE_BAND", Int, 100, "harness", "", "smoke harness only (cursor_guard.py): pixels from the screen edge that count as edge-safe"),
L(Exact1, "MC2_CURSOR_NO_PARK", Bool, false, "harness", "", "smoke harness only (scripts/smoke_lib/cursor_guard.py, tools/visual_verify/ab.py): leave the pointer where it is"),
L(Exact1, "MC2_CURSOR_PARK", Bool, false, "harness", "", "smoke harness only (cursor_guard.py): park the pointer at the screen edge before a run"),
L(Presence, "MC2_CURSOR_TARGET_TRACE", Bool, false, "code", "", ""),
G("MC2_DEBUG_OVERLAY_PREDICATE_MODE", Enum, "", "code", "", "", tokens("|Legacy")),
// history: docs/gate-history.md#MC2_DEBUG_RENDERER
L(Lead0, "MC2_DEBUG_RENDERER", Bool, false, "code", "", "Debug overlay renderer (GuiRuntime/EditorInspector.cpp)."),
L(Lead0, "MC2_DEBUG_RENDERER_TEST", Bool, false, "gameos", "", ""),
L(Presence, "MC2_DEBUG_SHADOW_FRUSTUM", Bool, false, "post", "", "[SHADOWFRUSTUM v1] VPL-#shadow: prove the static-shadow CLIPPER is correct (user suspected light/clipper; render-expert grep says it's feed-scope, not the..."),
L(Presence, "MC2_DEBUG_SHADOW_STATIC", Bool, false, "code", "", ""),
L(Presence, "MC2_DEBUG_SHADOW_ZRANGE", Bool, false, "post", "", "[SHADOWZRANGE v1] VPL-#10: prove the [0,1] ZERO_TO_ONE conversion is correct."),
// history: docs/gate-history.md#MC2_DEBUG_STATE_DUMP
L(Lead0, "MC2_DEBUG_STATE_DUMP", Bool, false, "gameos", "", "DEBUG-STATE-DUMP-1: write a JSON render-state snapshot to debug_state/latest_render_state.json every 300 frames (and at frame 1).", passthrough),
// history: docs/gate-history.md#MC2_DEBUG_STATE_DUMP_DIR
L(NonEmpty, "MC2_DEBUG_STATE_DUMP_DIR", Bool, false, "gameos", "", "DEBUG-STATE-DUMP-1: override output directory for MC2_DEBUG_STATE_DUMP.", passthrough),
// history: docs/gate-history.md#MC2_DEBUG_STATE_DUMP_HISTORY
L(Lead0, "MC2_DEBUG_STATE_DUMP_HISTORY", Bool, false, "gameos", "", "DEBUG-STATE-DUMP-2: enable rolling 8-slot history ring alongside latest_render_state.json.", passthrough),
L(Presence, "MC2_DEBUG_VIDEO", Bool, false, "code", "", ""),
// history: docs/gate-history.md#MC2_DEBUG_VIEW
P("MC2_DEBUG_VIEW", Int, 0, "code", "", "DEBUG-VIEW-SWEEP-1: =N (1..6) boot-seeds the global full-scene debug render view: 1=shadow-factor 2=normalized-depth 3=unlit-albedo 4=overdraw-proxy..."),
// history: docs/gate-history.md#MC2_DEBUG_VIEW_SWEEP
G("MC2_DEBUG_VIEW_SWEEP", Bool, false, "code", "", "DEBUG-VIEW-SWEEP-1: ='<startFrame>,<settleFrames>' frame-scheduled auto-sweep of the six MC2_DEBUG_VIEW views (missiongui.cpp update)."),
L(Lead1, "MC2_DECAL_GLPROBE", Bool, false, "terrain", "", "[TEMP DECAL_GLPROBE] eager-drain probe - env MC2_DECAL_GLPROBE=1."),
L(Presence, "MC2_DECOR_SHADOW_TRACE", Bool, false, "code", "", ""),
L(NonEmpty, "MC2_DEFS_ROOT", Bool, false, "unclassified", "", ""),
L(Presence, "MC2_DEPTHBIAS_CALIB", Bool, false, "terrain", "", ""),
L(Presence, "MC2_DEPTH_TRANSITION_PROBE", Bool, false, "terrain", "", "", passthrough),
L(Presence, "MC2_DESTROY_TRACE", Bool, false, "gameos", "", "", passthrough),
// history: docs/gate-history.md#MC2_DETERMINISTIC_RNG
L(Lead0, "MC2_DETERMINISTIC_RNG", Bool, false, "mech", "", "Route all randomness (incl.", passthrough),
L(Lead1, "MC2_DEV_SHELL", Bool, false, "unclassified", "", ""),
P("MC2_DEV_SHELL_PORT", Int, 0, "gameos", "", ""),  // default not derivable from the tree
G("MC2_DIAGNOSTIC_TRACE_FILE", String, "", "unclassified", "", "", passthrough),  // default not derivable from the tree
G("MC2_DIAG_TAGS", Enum, "", "gameos", "", "", passthrough | tokens("|*|none")),
// history: docs/gate-history.md#MC2_DIAG_VERBOSE
L(Lead0, "MC2_DIAG_VERBOSE", Bool, false, "gameos", "", "QUIET-DEFAULT-BUILD-1: ONE umbrella gate for the engine's undifferentiated developer chatter.", passthrough),
// history: docs/gate-history.md#MC2_DIRECT_FIRE_STRAIGHT
L(Exact1, "MC2_DIRECT_FIRE_STRAIGHT", Bool, false, "code", "", "Direct-fire de-curve: PPC/autocannon/gauss bolts fly STRAIGHT instead of re-tracking the moving target every frame (the janky homing curve).", passthrough),
L(Lead1, "MC2_DISABLE_GOSFX", Bool, false, "code", "", "", passthrough),
L(Lead1, "MC2_DPI_UNAWARE", Bool, false, "gameos", "", ""),
L(Lead1, "MC2_DRAW_PACKET_COALESCE_V5", Bool, false, "props", "", "", passthrough),
L(Lead1, "MC2_DRAW_PACKET_COALESCE_V5_TRACE", Bool, false, "props", "", "", passthrough),
L(Lead1, "MC2_DRAW_PACKET_COMPARE", Bool, false, "gameos", "", "", passthrough),
L(Lead1, "MC2_DRAW_PACKET_COMPARE_VERBOSE", Bool, false, "gameos", "", "", passthrough),
L(Lead1, "MC2_DRAW_PACKET_STATIC_PROP_V6_TRACE", Bool, false, "props", "", "", passthrough),
L(Lead1, "MC2_DRAW_PACKET_V3", Bool, false, "gameos", "", "", passthrough),
L(Lead1, "MC2_DRAW_PACKET_WARN", Bool, false, "gameos", "", ""),
// history: docs/gate-history.md#MC2_DYNAMIC_DECALS
L(Lead0P, "MC2_DYNAMIC_DECALS", Bool, false, "code", "", "Dynamic decal ring (mclib/dynamic_decal_ring.cpp): spawn()/gatherToDecalBatch() are no-ops when off."),
L(Lead0Kill, "MC2_EDGE_FOG", Bool, true, "post", "", ""),
P("MC2_EDGE_FOG_HEIGHT", Float, 0.0, "post", "", ""),  // default not derivable from the tree
P("MC2_EDGE_FOG_MAX", Float, 0.0, "post", "", ""),  // default not derivable from the tree
P("MC2_EDGE_FOG_START", Float, 0.0, "post", "", ""),  // default not derivable from the tree
L(Lead0Kill, "MC2_EDITOR_AUTODOCK", Bool, true, "code", "", ""),
L(Lead0, "MC2_EDITOR_BEAUTY_AUTOAPPLY", Bool, false, "editor", "", ""),
L(Lead0Kill, "MC2_EDITOR_DOCK", Bool, true, "code", "", ""),
P("MC2_EDITOR_FAR_CLIP", Float, 0.0, "code", "", ""),  // default not derivable from the tree
P("MC2_EDITOR_FPS_CAP", Int, -2, "editor", "", ""),
L(Lead0Kill, "MC2_EDITOR_GPU_PICK", Bool, true, "editor", "", ""),
L(Exact1, "MC2_EDITOR_GPU_TIMERS", Bool, false, "editor", "", ""),
L(Exact1, "MC2_EDITOR_MODE", Bool, false, "editor", "", ""),
L(Lead0Kill, "MC2_EDITOR_MOVER_REFRESH", Bool, true, "editor", "", ""),
P("MC2_EDITOR_PANEL_W", Int, 320, "editor", "", ""),
L(Presence, "MC2_EDITOR_PICK_TRACE", Bool, false, "editor", "", ""),
L(Exact1, "MC2_EDITOR_RTT", Bool, false, "code", "", ""),
L(Presence, "MC2_EDITOR_STATIC_PREWARM_OFF", Bool, false, "editor", "", ""),
L(Presence, "MC2_EDITOR_STATIC_PRIME_DIAG", Bool, false, "props", "", "MC2_EDITOR_STATIC_PRIME_DIAG=1 - compact aggregate DrawCmd census at finalize."),
L(Presence, "MC2_EDITOR_TRACE", Bool, false, "editor", "", ""),
G("MC2_EDITOR_UI_DEFS", Bool, true, "editor", "", "Mission Editor shell overlay is data/defs-first by default."),
L(Exact1, "MC2_EDITOR_WATCHDOG", Bool, false, "editor", "", ""),
G("MC2_EFFECT_ADMISSION_PREDICATE", Enum, "", "code", "", "", tokens("|legacy")),
L(Presence, "MC2_ENGINE_XL", Bool, false, "mech", "", ""),
L(Lead0, "MC2_EQUIPMENT_ADD", Bool, false, "code", "", ""),
G("MC2_EQUIPMENT_DEFINITION_SOURCE", Bool, false, "code", "", "", passthrough),
// history: docs/gate-history.md#MC2_FAR_BACKDROP_LIT
L(Lead0, "MC2_FAR_BACKDROP_LIT", Bool, false, "props", "", "FAR-BACKDROP-LIT-IMPL-1: night-shade substitution for far/impostor static-prop LODs (static_prop.vert).", passthrough),
// history: docs/gate-history.md#MC2_FAR_BACKDROP_LIT_FAR
P("MC2_FAR_BACKDROP_LIT_FAR", Float, 960.0, "props", "", "FAR-BACKDROP-LIT-IMPL-1: view-depth (clip.w, world units) where the night-shade fade completes (full substitution).", passthrough),
// history: docs/gate-history.md#MC2_FAR_BACKDROP_LIT_NEAR
P("MC2_FAR_BACKDROP_LIT_NEAR", Float, 700.0, "props", "", "FAR-BACKDROP-LIT-IMPL-1: view-depth (clip.w, world units) where the night-shade fade begins.", passthrough),
// history: docs/gate-history.md#MC2_FAR_BACKDROP_LIT_STRENGTH
P("MC2_FAR_BACKDROP_LIT_STRENGTH", Float, 1.0, "props", "", "FAR-BACKDROP-LIT-IMPL-1: maximum substitution amount scaling the distance fade (1.0 = full replace of the stale seed with night ambient; >1 exaggerates for...", passthrough),
// history: docs/gate-history.md#MC2_FAR_BACKDROP_LIT_TINT
L(NonEmpty, "MC2_FAR_BACKDROP_LIT_TINT", Bool, false, "props", "", "FAR-BACKDROP-LIT-IMPL-1: debug/exaggeration additive tint 'r,g,b' added to the substituted night shade so the affected far/impostor pixels are visually...", passthrough),
L(Lead0P, "MC2_FASTPATH_DROP_LOG", Bool, false, "terrain", "", "", passthrough),
L(Presence, "MC2_FF_TRACE", Bool, false, "unclassified", "", ""),
// history: docs/gate-history.md#MC2_FIRE_DECISION_TRACE
L(Lead0P, "MC2_FIRE_DECISION_TRACE", Bool, false, "mech", "", "ISSUE-17 fire-refusal instrumentation (default OFF = byte-identical; needs MC2_DIAG_TAGS=...,COMBAT).", passthrough),
// history: docs/gate-history.md#MC2_FIXTURE_ORDER_LIVENESS
L(Lead0P, "MC2_FIXTURE_ORDER_LIVENESS", Bool, false, "code", "", "META-FIX design F behavioral fixture (code/mission.cpp soak namespace; requires MC2_SOAK_AUTOWIN): at 20% of the autowin timer every armed home mech gets a...", passthrough),
// history: docs/gate-history.md#MC2_FLUSH_PROBE
L(Presence, "MC2_FLUSH_PROBE", Bool, false, "code", "", "TRACKV-CI-GATE-RESTORE-1: GPU pipeline flush probe via glFinish."),
// history: docs/gate-history.md#MC2_FOG_HORIZON_CLAMP
L(Lead0Kill, "MC2_FOG_HORIZON_CLAMP", Bool, true, "post", "", "FOG-HORIZON-CLAMP-1: reshape the out-of-bounds / edge fog to a horizon elevation profile (gos_postprocess)."),
// history: docs/gate-history.md#MC2_FOG_HORIZON_FADE_END
P("MC2_FOG_HORIZON_FADE_END", Float, 0.0, "post", "", "FOG-HORIZON-CLAMP-1: override the horizon-fog fade band END (elevation)."),  // default not derivable from the tree
// history: docs/gate-history.md#MC2_FOG_HORIZON_FADE_START
P("MC2_FOG_HORIZON_FADE_START", Float, 0.0, "post", "", "FOG-HORIZON-CLAMP-1: override the horizon-fog fade band START (elevation)."),  // default not derivable from the tree
L(Lead0, "MC2_FORCE_43", Bool, false, "gameos", "", "Default-OFF gate (matches the launcher toggle model: checked -> MC2_FORCE_43=1, unchecked -> unset).", passthrough),
// history: docs/gate-history.md#MC2_FORCE_ALL_VISIBLE
L(Lead0, "MC2_FORCE_ALL_VISIBLE", Bool, false, "code", "", "FORCE-ALL-VISIBLE-1: mark every unit (mechs, ground vehicles, elementals, all teams) fully visible and sensed, bypassing sensor / contact gating so nothing...", passthrough),
L(Lead0P, "MC2_FORCE_DYNAMIC_BUILDINGS", Bool, false, "props", "", ""),  // default not derivable from the tree
L(Lead0P, "MC2_FORCE_DYNAMIC_TREES", Bool, false, "props", "", ""),  // default not derivable from the tree
// history: docs/gate-history.md#MC2_FORCE_LOD
P("MC2_FORCE_LOD", Float, 0.0, "props", "", "TREE-OVERRIDE-LOD-MVP-1 (Task 5): force the active render-LOD index for override trees (0..MAX_LODS-1), clamped to the highest registered+available LOD."),  // default not derivable from the tree
L(Presence, "MC2_FORCE_TXM_REGEN", Bool, false, "terrain", "", "MC2_FORCE_TXM_REGEN=1 -> ignore the pre-cooked .txm composite cache and regenerate terrain texture composites from the source .tga (cement_N, overlays..."),
P("MC2_FPS_CAP", Int, 90, "gameos", "", "MC2_FPS_CAP: max FPS override.", passthrough),
L(Presence, "MC2_FRAMECAP_TRACE", Bool, false, "mech", "", ""),
L(Presence, "MC2_FRAMECTX_MISMATCH_FATAL", Bool, false, "gameos", "", ""),
L(Presence, "MC2_FRAMEGRAPH_AMBIENT_FATAL", Bool, false, "code", "", ""),
L(Lead0Kill, "MC2_FRAMEGRAPH_AMBIENT_GUARD", Bool, true, "code", "", ""),
L(Lead1, "MC2_FRAMEGRAPH_DRYRUN", Bool, false, "code", "", "", passthrough),
// history: docs/gate-history.md#MC2_FRAMEGRAPH_EXECUTOR
L(Lead1, "MC2_FRAMEGRAPH_EXECUTOR", Bool, false, "post", "", "RENDER-FRAME-GRAPH arc: top-level frame-graph executor ownership of pass begin/end (mclib/render_contract.cpp, GameOS/gameos/gos_postprocess.cpp).", passthrough),
L(Presence, "MC2_FRAMEGRAPH_REORDER_SPMECH", Bool, false, "code", "", "MEASURED-REORDER-SPMECH-1 (tier-C reorder EXPERIMENT, default-OFF): The two opaque flush UNITS below - StaticProp BatcherFlush (+Pass_SpColor timer) and the...", passthrough),
L(AtoiNZ, "MC2_FRAME_JOBS", Bool, false, "code", "", "", passthrough),
P("MC2_FRAME_JOBS_BATCH", Int, 64, "code", "", "", passthrough),
L(Lead1, "MC2_FRAME_JOBS_TOUCH", Bool, false, "code", "", "", passthrough),
G("MC2_FRAME_JOBS_TOUCH_DIAG", Bool, false, "props", "", ""),
L(AtoiNZ, "MC2_FRAME_JOBS_TRACE", Bool, false, "code", "", "", passthrough),
P("MC2_FRAME_JOBS_WORKERS", Int, 0, "code", "", "", passthrough),  // default not derivable from the tree
L(Exact1, "MC2_FRAME_PASS_STATS", Bool, false, "gameos", "", "", passthrough),
P("MC2_FRAME_PASS_STATS_EVERY", Int, 0, "gameos", "", "", passthrough),
// history: docs/gate-history.md#MC2_FREEZE_MOVERS_AT
P("MC2_FREEZE_MOVERS_AT", Int, -1, "code", "", "FREEZE-MOVERS-1: =<frame> freezes every mover the first frame g_mc2FrameCounter >= <frame>, WITHOUT needing the command channel (the 'freeze_movers on...", passthrough),
// history: docs/gate-history.md#MC2_FULLBAKE_DIAG
L(Lead1, "MC2_FULLBAKE_DIAG", Bool, false, "props", "", "SPOTLIGHT-DIRECTION-1 measurement instrument.", passthrough),
L(Lead0, "MC2_FXAA", Bool, false, "post", "", "", passthrough),
P("MC2_FXAA_EDGE_THRESHOLD", Float, 0.0, "post", "", "", passthrough),  // default not derivable from the tree
P("MC2_FXAA_EDGE_THRESHOLD_MIN", Float, 0.0, "post", "", "", passthrough),  // default not derivable from the tree
P("MC2_FXAA_SUBPIX", Float, 0.0, "post", "", "", passthrough),  // default not derivable from the tree
// history: docs/gate-history.md#MC2_FX_COST_SPLIT
L(Exact1, "MC2_FX_COST_SPLIT", Bool, false, "code", "", "FX cost-split instrumentation (default-off).", passthrough),
L(Presence, "MC2_FX_COUNT_LOG", Bool, false, "mech", "", "MC2_FX_COUNT_LOG=1: env-gated per-effect-site GOSFX draw counter for the Mech3DAppearance::render() effect sites.", passthrough),
// history: docs/gate-history.md#MC2_FX_DEFS
L(Presence, "MC2_FX_DEFS", Bool, false, "mech", "", "FX-DEFS-SIDECAR-1: per-effect JSON sidecar overlay for the gosFX spec catalog (mc2::particles::SpecLibrary).", passthrough),
// history: docs/gate-history.md#MC2_FX_FORCE_SPAWN
L(Lead0P, "MC2_FX_FORCE_SPAWN", Bool, false, "code", "", "FX-FORCE-SPAWN v1 fixture: default-OFF.", passthrough),
L(Lead1, "MC2_FX_TRACE", Bool, false, "code", "", "", passthrough),
G("MC2_GAMEOS_UI_DEFS", Bool, true, "editor", "", "The data/defs GameOS UI path is the replacement path, not an opt-in demo."),
L(Exact1, "MC2_GAMEPLAY_PICK_SELFTEST", Bool, false, "code", "", ""),
L(Presence, "MC2_GEOM_PHASE_SPLIT", Bool, false, "terrain", "", "", passthrough),
// history: docs/gate-history.md#MC2_GLSTATEGUARD_LOG
L(Presence, "MC2_GLSTATEGUARD_LOG", Bool, false, "gameos", "", "GLSTATEGUARD-SLICE-1: leak-visibility for mc2gl::GlScopedSsboBinding (gl_state_guard.h).", passthrough),
// history: docs/gate-history.md#MC2_GLSTATEGUARD_TERRAIN
L(DefOnAtoiNZ, "MC2_GLSTATEGUARD_TERRAIN", Bool, true, "terrain", "", "GLSTATEGUARD-SLICE-2: kill-switch for the RAII depth/blend/cull ownership of the terrain LOD chunk draw (gos_TerrainLodChunk_SubmitDrawCommands via..."),
P("MC2_GLTF_AXIS", Int, 0, "props", "", "Coordinate transforms (spec ?6)."),
P("MC2_GLTF_GROUND", Int, 2, "code", "", ""),
P("MC2_GLTF_YAW_DEG", Float, 0.0, "props", "", ""),
P("MC2_GLTF_YOFF", Float, 0.0, "code", "", ""),
L(Presence, "MC2_GL_DEBUG", Bool, false, "gameos", "", "Install GL debug callback only when MC2_GL_DEBUG is set.", passthrough),
L(Presence, "MC2_GL_DEBUG_FATAL", Bool, false, "gameos", "", "Tier 1.2 (docs/testing-strategy.md): opt-in fatal abort on GL_DEBUG_SEVERITY_HIGH.", passthrough),
L(Presence, "MC2_GL_ERROR_DRAIN_SILENT", Bool, false, "gameos", "", "GL_ERROR is default-on; the env var suppresses it."),
L(Presence, "MC2_GOM_RECON", Bool, false, "code", "", "GOM Recon: object type classification & mutation tracking (read-only)."),
// history: docs/gate-history.md#MC2_GOM_UPDATE_COST_SPLIT
L(Lead0P, "MC2_GOM_UPDATE_COST_SPLIT", Bool, false, "code", "", "GOM_UPDATE_COST v1 (framebudget): =1 emits one coarse per-phase GameObjectManager update CPU-cost summary to stderr (objmgr.cpp).", passthrough),
L(Lead1, "MC2_GOSFX_GROUP_LOG", Bool, false, "gameos", "", ""),
L(Presence, "MC2_GPUBUF_COUNTER", Bool, false, "gameos", "", "GPU-UPDATE-BUFFER-COUNTER-1: independent default-OFF gate."),
// history: docs/gate-history.md#MC2_GPUBUF_LIGHT_GROWONCE
L(Presence, "MC2_GPUBUF_LIGHT_GROWONCE", Bool, false, "gameos", "", "LIGHT-GROW-ONCE-SUBDATA-1 (AMD-safe intermediate for the light SSBO orphan churn; NOT the full GpuStorageRing ring)."),
// history: docs/gate-history.md#MC2_GPUBUF_RESIDENCY
L(Presence, "MC2_GPUBUF_RESIDENCY", Bool, false, "gameos", "", "GPU-BUFFER-WRAPPER-TIER0-HUD-1: every ~300 frames list the live HUD ring buffers ([GPUBUF v1] residency tag/kind/frames/vbBytes/ibBytes/liveFences)."),
// history: docs/gate-history.md#MC2_GPUBUF_RING
L(Presence, "MC2_GPUBUF_RING", Bool, false, "gameos", "", "GPU-BUFFER-WRAPPER-TIER0-HUD-1: route the fixed-capacity gosMesh HUD/2D meshes (gameos_graphics.cpp draw/drawIndexed) through a 3-frame persistent-coherent..."),
// history: docs/gate-history.md#MC2_GPUBUF_RING_FORCE_WRAP
L(Presence, "MC2_GPUBUF_RING_FORCE_WRAP", Bool, false, "gameos", "", "GPU-BUFFER-WRAPPER-TIER0-HUD-1 NEGATIVE TEST: deterministically advance the HUD ring twice (beginFrame without an intervening endFrame) so the..."),
L(Presence, "MC2_GPUPROPS_TRACE", Bool, false, "props", "", "[GPUPROPS v1] setup-path instrumentation (internal; path tag lives at file scope).", passthrough),
L(Lead0Kill, "MC2_GPU_CULL", Bool, true, "unclassified", "", "", passthrough),
L(Lead0P, "MC2_GPU_CULL_AABB_PARITY", Bool, false, "gameos", "", "", passthrough),
L(Presence, "MC2_GPU_CULL_COMPUTE_TRACE", Bool, false, "gameos", "", "Trace macro (env-gated, default off)", passthrough),
L(Lead0Kill, "MC2_GPU_CULL_CONSERVATIVE_OR", Bool, true, "gameos", "", ""),
L(Presence, "MC2_GPU_CULL_DUMP_SHADER", Bool, false, "gameos", "", "Debug: dump concatenated source to file for inspection."),
L(Presence, "MC2_GPU_CULL_FORCE_FENCE_NOT_READY", Bool, false, "gameos", "", "", passthrough),
P("MC2_GPU_CULL_FRUSTUM_DILATION", Float, 0.0, "gameos", "", ""),  // default not derivable from the tree
L(Presence, "MC2_GPU_CULL_LIFECYCLE", Bool, false, "mech", "", "C3: env-gated lifecycle routing killswitch (same env var as objmgr.cpp).", passthrough),
L(Presence, "MC2_GPU_CULL_LIFECYCLE_TRACE", Bool, false, "mech", "", "MC2_GPU_CULL_LIFECYCLE_TRACE=1: verbose per-actor lifecycle boundary prints.", passthrough),
L(Presence, "MC2_GPU_CULL_OWNERSHIP_PARITY", Bool, false, "gameos", "", "Ownership-parity oracle (Slice A, measurement-only, default off).", passthrough),
L(Lead0Kill, "MC2_GPU_CULL_READBACK", Bool, true, "gameos", "", "PERF-OBJECT-ITER-GPU-PORT-1: default-ON now that substrate+coalesce are stable.", passthrough),
L(Presence, "MC2_GPU_CULL_READBACK_TRACE", Bool, false, "gameos", "", "Env-gated trace macro.", passthrough),
L(Presence, "MC2_GPU_CULL_STATIC_EAGER_LIGHT_BAKE", Bool, false, "props", "", ""),
L(Presence, "MC2_GPU_CULL_STATIC_FROZEN_ORDER_ORACLE", Bool, false, "props", "", "Order-validation oracle (gated): for each placed record, verify its actor center is near the recipe's own leaf world position (within the bounding sphere)."),
L(Presence, "MC2_GPU_CULL_STATIC_FROZEN_RECORDS", Bool, false, "props", "", "M1 FROZEN-STATIC-CULL-RECORDS (gate MC2_GPU_CULL_STATIC_FROZEN_RECORDS)."),
// history: docs/gate-history.md#MC2_GPU_CULL_STATIC_LIGHT_ZERO_PROBE
L(Presence, "MC2_GPU_CULL_STATIC_LIGHT_ZERO_PROBE", Bool, false, "props", "", "Static-prop light-bake observation gate, default OFF."),
L(Lead0Kill, "MC2_GPU_CULL_SUBSTRATE", Bool, true, "gameos", "", "Default ON; MC2_GPU_CULL_SUBSTRATE=0 opts out.", passthrough),
L(Presence, "MC2_GPU_CULL_SUBSTRATE_TRACE", Bool, false, "gameos", "", "MC2_GPU_CULL_SUBSTRATE_TRACE=1 - verbose per-frame lifecycle prints.", passthrough),
L(Presence, "MC2_GPU_DEBUG_NAMES", Bool, false, "gameos", "", "", passthrough),
L(Lead0Kill, "MC2_GPU_DRIVEN", Bool, true, "unclassified", "", "", passthrough),
L(Lead0Kill, "MC2_GPU_DRIVEN_OVERLAY", Bool, true, "gameos", "", "", passthrough),
L(Lead1, "MC2_GPU_DRIVEN_PARITY", Bool, false, "gameos", "", "", passthrough),
L(Lead0Kill, "MC2_GPU_DRIVEN_TERRAIN_SOLID", Bool, true, "gameos", "", "", passthrough),
L(Lead1, "MC2_GPU_DRIVEN_TRACE", Bool, false, "gameos", "", "", passthrough),
L(Lead0Kill, "MC2_GPU_DRIVEN_WATER", Bool, true, "gameos", "", "", passthrough),
L(Lead0, "MC2_GPU_GROUND_PICK_PARITY", Bool, false, "code", "", "", passthrough),
L(Presence, "MC2_GPU_GROUND_PICK_PARITY_CENTER", Bool, false, "code", "", "Center-probe knob: unattended smoke parks the cursor at (0,0), so the primary cursor-driven oracle never samples real terrain (compared=0)."),
P("MC2_GPU_GROUND_PICK_PARITY_SAMPLE", Int, 30, "code", "", "", passthrough),
G("MC2_GPU_GROUND_PICK_PARITY_TAG", String, "", "code", "", "", passthrough),  // default not derivable from the tree
L(Lead0Kill, "MC2_GPU_GV_SHADOW_SKIP", Bool, true, "mech", "", ""),
// history: docs/gate-history.md#MC2_GPU_MECHS
L(Lead0Kill, "MC2_GPU_MECHS", Bool, true, "mech", "", "[RETIRED] GPU mech batcher (gos_mech_batcher)."),
// history: docs/gate-history.md#MC2_GPU_MECH_CULL
L(Lead0Kill, "MC2_GPU_MECH_CULL", Bool, true, "mech", "", "Slice C1 (gos_mech_batcher.cpp): render-only mech GPU cull -- skip GPU mech submit for actors the GPU lagged-readback says were invisible last frame (CPU..."),
// history: docs/gate-history.md#MC2_GPU_MECH_FAST_TRANSFORM
L(Lead0Kill, "MC2_GPU_MECH_FAST_TRANSFORM", Bool, true, "mech", "", "Slice C3-revised (gos_mech_batcher.cpp): body-only fast transform path for GPU-driven mechs."),
// history: docs/gate-history.md#MC2_GPU_MECH_LEAF_SKIP
L(Lead0Kill, "MC2_GPU_MECH_LEAF_SKIP", Bool, true, "mech", "", "Slice D-leaf-skip-v2 (gos_mech_batcher.cpp): strip per-leaf body of mechShape->TransformMultiShape* work when the leaf is provably unaffected."),
// history: docs/gate-history.md#MC2_GPU_MECH_SHADOW_FAST_TRANSFORM
L(Lead0Kill, "MC2_GPU_MECH_SHADOW_FAST_TRANSFORM", Bool, true, "mech", "", "Slice C3-shadow (gos_mech_batcher.cpp): fast-transform variant for the mech shadow-caster callsite."),
// history: docs/gate-history.md#MC2_GPU_OBJECTS
L(Lead0P, "MC2_GPU_OBJECTS", Bool, false, "unclassified", "", "[RETIRED] Indirect-draw static-prop batcher (gos_static_prop_batcher).", passthrough),
// history: docs/gate-history.md#MC2_GPU_PARTICLES
L(Lead1, "MC2_GPU_PARTICLES", Bool, false, "code", "", "Master gate for GPU billboard particles (mclib/particles/batcher.cpp initialize_env).", passthrough),
L(Lead1, "MC2_GPU_PARTICLES_LOG", Bool, false, "code", "", "", passthrough),
L(Lead0, "MC2_GPU_PICK_HOVER", Bool, false, "code", "", ""),
L(Lead0, "MC2_GPU_PICK_HOVER_TRACE", Bool, false, "code", "", ""),
P("MC2_GPU_PROPS_DEBUG_MODE", Int, 0, "props", "", ""),  // default not derivable from the tree
L(Lead1, "MC2_GPU_SYNC_TRACE", Bool, false, "gameos", "", ""),
L(Lead1, "MC2_GPU_TRAIL_DISABLE", Bool, false, "code", "", ""),
L(Lead0P, "MC2_GROUND_CONTACT_BLOB", Bool, false, "code", "", "", passthrough),
// history: docs/gate-history.md#MC2_GV_LIGHTSOUT_CLEAR
L(Lead0, "MC2_GV_LIGHTSOUT_CLEAR", Bool, false, "mech", "", "VEHICLE-NIGHT-LIT-2: GVAppearance::update (mclib/gvactor.cpp) calls gvShape->SetLightsOut(true) when a ground vehicle is OBJECT_STATUS_DESTROYED or...", passthrough),
L(Lead0, "MC2_HARNESS_ALLOW_FOREGROUND", Bool, false, "harness", "", "smoke harness only (nofocus.py, isolation.py): a user-present session opts in to foreground launches"),
L(Lead0, "MC2_HARNESS_PARK", Bool, false, "harness", "", "smoke harness only (nofocus.py): park the launched window"),
L(Presence, "MC2_HAZE_PARITY", Bool, false, "terrain", "", "Step 7 (VPL retirement): MC2_HAZE_PARITY probe - env-gated default-off."),
L(Lead0Kill, "MC2_HDRI_BC6H", Bool, true, "gameos", "", ""),
L(Lead0Kill, "MC2_HDRI_SKY", Bool, true, "post", "", ""),
P("MC2_HDRI_SKY_AZ_OFFSET", Float, 0.0, "post", "", ""),
L(Lead0P, "MC2_HDRI_SKY_FRAME_FIX", Bool, false, "post", "", ""),
L(Lead0P, "MC2_HDRI_SKY_UV_DEBUG", Bool, false, "post", "", ""),
L(Presence, "MC2_HEARTBEAT", Bool, false, "gameos", "", "", passthrough),
P("MC2_HEIGHT", Int, 0, "gameos", "", "", passthrough),
P("MC2_HITCH_MS", Float, 0.0, "gameos", "", "", passthrough),  // default not derivable from the tree
L(Presence, "MC2_HITCH_TRACE", Bool, false, "gameos", "", "", passthrough),
G("MC2_HOSTSVC_DEFINITELY_UNSET_KEY_XYZ", Bool, false, "unclassified", "", ""),  // default not derivable from the tree
L(Presence, "MC2_HOTKEY_TRACE", Bool, false, "code", "", "Env-gated lifecycle trace for the rebind-to-occupied-key crash."),
// history: docs/gate-history.md#MC2_HUD_IMGUI_TABS
G("MC2_HUD_IMGUI_TABS", Bool, false, "code", "", "IN-MISSION-HUD-IMGUI-SCOPE-1 slice C: data-driven command-bar tab registry (code/hudtabregistry.{h,cpp} + wiring in code/controlgui.cpp)."),
G("MC2_HUD_IMGUI_TACMAP", Bool, false, "code", "", ""),  // default not derivable from the tree
// history: docs/gate-history.md#MC2_HUD_READABILITY_V1
L(Lead0, "MC2_HUD_READABILITY_V1", Bool, false, "unclassified", "", "IN-MISSION-HUD-IMGUI-SCOPE-1 slice A: data-first HUD readability."),
P("MC2_HUD_SCALE", Float, 0.0, "gameos", "", ""),  // default not derivable from the tree
G("MC2_HUD_SIM_SPEED", Bool, false, "code", "", "SIM-SPEED-BUTTON-1: double-gated sim-speed tab (MC2_HUD_SIM_SPEED; needs MC2_HUD_IMGUI_TABS like every registry consumer)."),  // default not derivable from the tree
// history: docs/gate-history.md#MC2_HUD_TABS_TEST
G("MC2_HUD_TABS_TEST", Bool, false, "code", "", "IN-MISSION-HUD-IMGUI-SCOPE-1 slice C witness: register a 4th 'test' tab (no legacy button; ImGui chrome via HudImGui::hudShrinkPoint + hudToDisplay, trivial..."),
// history: docs/gate-history.md#MC2_HZB_BUILD
L(Lead0, "MC2_HZB_BUILD", Bool, false, "post", "", "TRACKRV-HZB-VISIBILITY-OPUS-1: build a reverse-Z Hi-Z depth pyramid from MainDepth each frame via a custom fragment MIN-reduction pass (hzb_reduce.frag)...", passthrough),
L(Presence, "MC2_HZB_FORCE_HORIZON", Bool, false, "code", "", ""),
P("MC2_HZB_FORCE_HORIZON_ALT", Float, 150.0, "code", "", ""),
P("MC2_HZB_FORCE_HORIZON_ANGLE", Float, 12.0, "code", "", ""),
L(Presence, "MC2_HZB_FORCE_HORIZON_POS", Bool, false, "code", "", "Position override is opt-in (MC2_HZB_FORCE_HORIZON_POS=1): place the camera at a map corner."),
P("MC2_HZB_FORCE_HORIZON_ROT", Float, 135.0, "code", "", ""),
P("MC2_HZB_FORCE_HORIZON_X", Float, 0.9, "code", "", "Corner position as a fraction of the half-map extent (map is centered at origin: vegHalfMap = worldUnitsMapSide*0.5)."),
P("MC2_HZB_FORCE_HORIZON_Y", Float, -0.9, "code", "", ""),
// history: docs/gate-history.md#MC2_HZB_PROBE
L(Lead0, "MC2_HZB_PROBE", Bool, false, "post", "", "TRACKRV-HZB-VISIBILITY-OPUS-1: diagnostic-only HZB occlusion probe.", passthrough),
G("MC2_HZB_VIEW_FILE", Bool, false, "code", "", "HZB validation knob (debug only): force a low across-map HORIZON view so static props occlude each other."),  // default not derivable from the tree
L(Lead1, "MC2_IBL_ASSET_STRICT", Bool, false, "unclassified", "", ""),
// history: docs/gate-history.md#MC2_ICON_FAILLOUD
L(Lead0, "MC2_ICON_FAILLOUD", Bool, false, "mech", "", "META-FIX design B PART 3 (code/mechicon.cpp iconTileChecked at the three CPU icon-tile blits): default OFF. =1 substitutes a solid MAGENTA tile when an icon...", passthrough),
// history: docs/gate-history.md#MC2_IFACE_COST_SPLIT
L(Presence, "MC2_IFACE_COST_SPLIT", Bool, false, "code", "", "MISSION-INTERFACE-PERF-1: chrono cost-split of MissionInterfaceManager::update() (the GameLogic.Mission.Interface Tracy zone) incl.", passthrough),
// history: docs/gate-history.md#MC2_IMGUI
L(Lead0Kill, "MC2_IMGUI", Bool, true, "code", "", "ImGui overlay (GuiRuntime/GuiRuntime.cpp)."),
L(Lead1, "MC2_IMGUI_DEMO", Bool, false, "code", "", ""),
// history: docs/gate-history.md#MC2_IMGUI_INSPECTOR
L(Lead0Kill, "MC2_IMGUI_INSPECTOR", Bool, true, "code", "", "ImGui inspector panel (GuiRuntime/EditorInspector.cpp)."),
P("MC2_IMPORTER_DIR", Int, 0, "unclassified", "", ""),
// history: docs/gate-history.md#MC2_IMPOSTOR_DIST
P("MC2_IMPOSTOR_DIST", Float, 800.0, "props", "", "Static-prop / tree impostor LOD switch distance in world units (mclib/bdactor.cpp TreeAppearance::selectActiveLOD).", passthrough),
L(Presence, "MC2_INVIEW_CONFLATION_TRACE", Bool, false, "code", "", "alpha-Stage 1 ?5 Stage 0: candidate-predicate disagreement probe.", passthrough),
G("MC2_ISOLATION_DESKTOP", String, "", "harness", "", "smoke harness only (isolation.py): name of the private desktop object"),
G("MC2_ISOLATION_MIN", String, "", "harness", "", "smoke harness only (isolation.py): lowest acceptable isolation tier (winsta|desktop)"),
G("MC2_ISOLATION_WINSTA", String, "", "harness", "", "smoke harness only (isolation.py): name of the private window-station object"),
L(Presence, "MC2_JUMPJETS_ALL", Bool, false, "code", "", ""),
L(Presence, "MC2_LAUNCHED", Bool, false, "gameos", "", ""),
L(Lead0Kill, "MC2_LAUNCHER_ENV_JSON", Bool, true, "gameos", "", ""),
L(Lead0P, "MC2_LEGACY_INSTANCE_POOLS", Bool, false, "props", "", ""),
// history: docs/gate-history.md#MC2_LEGACY_LIT_MC2_FRAME
L(Lead0Kill, "MC2_LEGACY_LIT_MC2_FRAME", Bool, true, "code", "", "[DEFAULT-ON as of VEHICLE-LIGHTING-PATH-1; MC2_LEGACY_LIT_MC2_FRAME=0 reverts.", passthrough),
L(Presence, "MC2_LEGACY_LOAD_BANNER", Bool, false, "code", "", ""),
// history: docs/gate-history.md#MC2_LETTERBOX_OPAQUE
L(Lead0Kill, "MC2_LETTERBOX_OPAQUE", Bool, true, "code", "", "Cinematic letterbox bars are drawn OPAQUE black; only their EXTENT animates.", passthrough),
L(Lead0Kill, "MC2_LIGHTBAKE", Bool, true, "code", "", ""),
// history: docs/gate-history.md#MC2_LIGHTBAKE_PARITY
L(Presence, "MC2_LIGHTBAKE_PARITY", Bool, false, "code", "", "STATICPROP-PERMANENT-INSTANCE-LIGHTS-1: baked permanent light record == gathered transient record byte/hash parity."),
// history: docs/gate-history.md#MC2_LIGHTBAKE_REBAKE_ON_READY
L(Lead0Kill, "MC2_LIGHTBAKE_REBAKE_ON_READY", Bool, true, "props", "", "LIGHTBAKE-REBAKE-ON-READY-1: soft-invalidate the per-recipe static-light bake cache once, 8 frames after mission-ready, so recipes baked during boot/load...", passthrough),
// history: docs/gate-history.md#MC2_LIGHTBAKE_STABILITY
L(Presence, "MC2_LIGHTBAKE_STABILITY", Bool, false, "code", "", "STATICPROP-PERMANENT-INSTANCE-LIGHTS-1: per-instance lightDataIndex permanence/stability proof."),
L(Lead0Kill, "MC2_LIGHTBRIDGE", Bool, true, "code", "", ""),
G("MC2_LIGHTBRIDGE_STABLE_SKIP", Bool, false, "props", "", ""),
G("MC2_LIGHTBRIDGE_STABLE_SKIP_DIAG", Bool, false, "props", "", ""),
L(Lead0P, "MC2_LIGHTGRID_BUILD", Bool, false, "gameos", "", ""),
L(Lead0P, "MC2_LIGHTGRID_PLANT", Bool, false, "gameos", "", ""),
L(Lead0P, "MC2_LIGHTGRID_VERIFY", Bool, false, "gameos", "", ""),
// history: docs/gate-history.md#MC2_LIGHTING_DEBUG_VIEW
G("MC2_LIGHTING_DEBUG_VIEW", Enum, "", "gameos", "", "DEBUG-VIEW-SWEEP-1: replace lit shading with a diagnostic channel across terrain, static props and mechs.", passthrough | tokens("|0|albedo|ambient|final|lightcount|lightindex|normal|off|overbright|shadow|sun")),
L(Presence, "MC2_LIGHTING_LINEAR_AUDIT", Bool, false, "post", "", "LINEAR-COLOR-AUDIT-1: one-shot runtime color-pipeline probe."),
G("MC2_LIGHTING_SHADOW_PREDICATE_MODE", Enum, "", "code", "", "", tokens("|Legacy")),
L(Presence, "MC2_LIGHTSLOT_TRACE", Bool, false, "props", "", ""),
L(Presence, "MC2_LIGHTSSBO_TRACE", Bool, false, "mech", "", ""),
P("MC2_LIGHT_BILLBOARD_FACING", Int, 0, "props", "", "", passthrough),
L(Lead0, "MC2_LIGHT_BILLBOARD_TWOSIDED", Bool, false, "code", "", "", passthrough),
L(Exact1, "MC2_LIGHT_CLAMP_FIXTURE", Bool, false, "code", "", "MC2_LIGHT_CLAMP_FIXTURE (default OFF) - synthetic injector of deterministic extra point lights into slots [populated..32) to prove slots >16 ever render."),
L(Exact1, "MC2_LIGHT_CLAMP_PROBE", Bool, false, "code", "", "MC2_LIGHT_CLAMP_PROBE (default OFF) - pure observation."),
L(Lead1, "MC2_LIGHT_CONE_AIM_FLIP", Bool, false, "code", "", "", passthrough),
L(Lead1, "MC2_LIGHT_CONE_DIAG", Bool, false, "props", "", "", passthrough),
L(Lead0Kill, "MC2_LIGHT_CONE_GLOW", Bool, true, "code", "", "", passthrough),
P("MC2_LIGHT_CONE_GLOW_ALPHA", Float, 0.0, "code", "", "", passthrough | clampRange(0.0, 255.0)),  // default not derivable from the tree
// history: docs/gate-history.md#MC2_LIGHT_CONE_NODES
L(Lead0Kill, "MC2_LIGHT_CONE_NODES", Bool, true, "code", "", "LIGHT-CONE-MODELS-1: DEFAULT-ON.", passthrough),
P("MC2_LIGHT_CONE_SPILL", Float, 40.0, "code", "", "", passthrough | clampRange(0.0, 3.0)),
L(Lead1, "MC2_LIGHT_CONE_VIS_GATE", Bool, false, "code", "", "", passthrough),
L(Presence, "MC2_LIGHT_COST_SPLIT", Bool, false, "terrain", "", "", passthrough),
L(Lead0Kill, "MC2_LIGHT_CULL_RADIUS", Bool, true, "code", "", "", passthrough),
// history: docs/gate-history.md#MC2_LIGHT_GRID
L(Lead0Kill, "MC2_LIGHT_GRID", Bool, true, "terrain", "", "LIGHT-CLUSTER-PERF-1: world-space XY light GRID over the per-frame terrain light pool.", passthrough),
// history: docs/gate-history.md#MC2_LIGHT_GRID_DIM
P("MC2_LIGHT_GRID_DIM", Int, 64, "terrain", "", "LIGHT-CLUSTER-PERF-1 knob: cells per axis for the light grid (grid is DIM x DIM over the XY bbox of this frame's packed lights).", passthrough),
L(Lead1, "MC2_LIGHT_MOVER_VIS_GATE", Bool, false, "code", "", "", passthrough),
// history: docs/gate-history.md#MC2_LIGHT_POOL_COMPRESS
L(Lead0Kill, "MC2_LIGHT_POOL_COMPRESS", Bool, true, "terrain", "", "NIGHT-LIGHT-POOLS-3: tone-compress the accumulated terrain light-pool sum (chunk + overlay frag): 1-exp(-1.6*sum) instead of min(sum,1).", passthrough),
L(Lead1, "MC2_LIGHT_POOL_DUMP", Bool, false, "terrain", "", "", passthrough),
// history: docs/gate-history.md#MC2_LIGHT_POOL_MAX
P("MC2_LIGHT_POOL_MAX", Int, 0, "terrain", "", "LIGHT-POOL-CAP-1: per-mission slot count of the terrain light pool (gos_terrain_lighting.cpp mission_init).", passthrough),
P("MC2_LIGHT_POOL_MOVER_COLOR", Int, 0, "code", "", "", passthrough),  // default not derivable from the tree
P("MC2_LIGHT_POOL_MOVER_DIM", Float, 0.25, "terrain", "", "", passthrough),
P("MC2_LIGHT_POOL_MOVER_FWD", Float, 24.0, "code", "", "", passthrough | range(0.0, 120.0)),
L(Lead0Kill, "MC2_LIGHT_POOL_MOVER_MAXBLEND", Bool, true, "terrain", "", "", passthrough),
P("MC2_LIGHT_POOL_MOVER_RADIUS", Float, 0.6, "terrain", "", "", passthrough),
L(Lead0Kill, "MC2_LIGHT_POOL_OBJECTS", Bool, true, "terrain", "", "", passthrough),
P("MC2_LIGHT_POOL_OBJ_SCALE", Float, 0.7, "terrain", "", "", passthrough | range(0.0, 2.0)),
L(Lead0Kill, "MC2_LIGHT_POOL_SOFT", Bool, true, "terrain", "", "", passthrough),
L(Lead0Kill, "MC2_LIGHT_POOL_VIS_GATE", Bool, true, "terrain", "", ""),
// history: docs/gate-history.md#MC2_LIGHT_POS_COLUMN_FIX
L(Lead0Kill, "MC2_LIGHT_POS_COLUMN_FIX", Bool, true, "code", "", "[DEFAULT-ON as of VEHICLE-LIGHTING-PATH-1; MC2_LIGHT_POS_COLUMN_FIX=0 reverts.", passthrough),
// history: docs/gate-history.md#MC2_LIGHT_PREFIX_GPU_COPY
L(Lead0P, "MC2_LIGHT_PREFIX_GPU_COPY", Bool, false, "gameos", "", "LIGHT-PREFIX-GPU-COPY-1 (TXMMGR-PERF-EASYWINS-1): keep a VRAM stash of the immutable static light prefix [0..S) and glCopyBufferSubData it into the...", passthrough),
// history: docs/gate-history.md#MC2_LIGHT_REALISM
L(Lead0Kill, "MC2_LIGHT_REALISM", Bool, true, "terrain", "", "LIGHT-REALISM-1: night light-pool + cone-glow realism reshape.", passthrough),
P("MC2_LIGHT_REALISM_BEACON", Float, 0.0, "code", "", "", passthrough | clampRange(0.0, 255.0)),
P("MC2_LIGHT_REALISM_BEACON_RADIUS", Float, 70.0, "code", "", "", passthrough | clampRange(1.0, 200.0)),
P("MC2_LIGHT_REALISM_CONE_FEATHER", Float, 0.4, "code", "", "", passthrough | clampRange(0.0, 8.0)),
P("MC2_LIGHT_REALISM_CONE_OPACITY", Float, 0.0, "code", "", "", passthrough | clampRange(0.0, 4.0)),  // default not derivable from the tree
P("MC2_LIGHT_REALISM_POOL_CORE", Float, 1.0, "terrain", "", "", passthrough | range(0.0, 1.0)),
P("MC2_LIGHT_REALISM_POOL_SPILL", Float, 0.25, "terrain", "", "", passthrough | range(0.25, 4.0)),
L(Exact1, "MC2_LIGHT_ROW_CENSUS", Bool, false, "code", "", "MC2_LIGHT_ROW_CENSUS (default OFF) - LIGHT-POS-COMBAT-1."),
// history: docs/gate-history.md#MC2_LIGHT_ROW_PURE
L(Exact1, "MC2_LIGHT_ROW_PURE", Bool, false, "code", "", "[DEFAULT-OFF, KILLSWITCH INVERTED: MC2_LIGHT_ROW_PURE=1 opts IN to the pure producer; unset/0 is the legacy in-place packing loop in GatherLightsParameters...", passthrough),
// history: docs/gate-history.md#MC2_LIGHT_ROW_PURE_VERIFY
L(Exact1, "MC2_LIGHT_ROW_PURE_VERIFY", Bool, false, "code", "", "[DEFAULT-OFF.", passthrough),
L(Lead0, "MC2_LIGHT_UPLOAD_STATS", Bool, false, "gameos", "", ""),
// history: docs/gate-history.md#MC2_LINKUP_TRACE
L(Presence, "MC2_LINKUP_TRACE", Bool, false, "mech", "", "ISSUE-50 diagnostics (code/mech.cpp BattleMech::update): for every off-GUI mech, periodically (~1/s) logs [LINKUP] part/name/status/los, plus [LINKUP]...", passthrough),
// history: docs/gate-history.md#MC2_LOAD_RACE_DELAY_ATLAS
L(Presence, "MC2_LOAD_RACE_DELAY_ATLAS", Bool, false, "terrain", "", "gos_terrain_indirect.cpp negative control for the mission-load residency-race probes: when set (any value), BuildColormapAtlas() defers the real GL atlas..."),
// history: docs/gate-history.md#MC2_LOAD_RACE_PROBE_FRAMES
L(Presence, "MC2_LOAD_RACE_PROBE_FRAMES", Bool, false, "gameos", "", "gameosmain.cpp: overrides the comma-separated list of frame numbers at which the mission-load residency-race one-shot diagnostics probe fires (see..."),
L(Presence, "MC2_LOAD_RACE_TRACE", Bool, false, "code", "", "MC2_LOAD_RACE_TRACE: gated, one-shot mission-load residency-race diagnostics."),
P("MC2_LOAD_RACE_VERDICT_FRAME", Int, 0, "gameos", "", ""),  // default not derivable from the tree
// history: docs/gate-history.md#MC2_LOG
G("MC2_LOG", String, "", "gameos", "", "Master stdout log gate (gameosmain.cpp, right after setvbuf).", passthrough),
L(Presence, "MC2_LOG_CURSOR", Bool, false, "code", "", "---- MC2_LOG_CURSOR diagnostics (instrumentation only) ---- Logistics::start does NOT call mouseOn() (it is commented out) -- it relies on drawMouse already..."),
G("MC2_LOG_DECORATE", String, "", "gameos", "", ""),  // default not derivable from the tree
// history: docs/gate-history.md#MC2_LOG_FILE_RESOLVE
P("MC2_LOG_FILE_RESOLVE", Int, 0, "code", "", "META-FIX design E tier 3 (mclib/file.cpp, parsed at InitModSearchPaths): file-resolution ladder diagnostics.", passthrough),
L(Presence, "MC2_LOG_LOGISTICS", Bool, false, "mech", "", "", passthrough),
L(Presence, "MC2_LOG_LOGISTICS_FLOW", Bool, false, "code", "", ""),
L(Presence, "MC2_LOG_MECHICON", Bool, false, "mech", "", ""),
L(Presence, "MC2_LOG_MECH_ICON", Bool, false, "mech", "", ""),
L(Presence, "MC2_LOG_PILOTS", Bool, false, "code", "", "PILOTS-ROSTER diagnostic (MC2_LOG_PILOTS=1): how many pilots loaded + the roster path."),
L(Presence, "MC2_LOG_PREVIEW", Bool, false, "mech", "", ""),
L(Presence, "MC2_LOG_STRINGS", Bool, false, "gameos", "", ""),
L(Lead0Kill, "MC2_LOWCAM_OBJ_NEARPAD", Bool, true, "code", "", "Default ON for this low-camera build; set MC2_LOWCAM_OBJ_NEARPAD=0 to disable."),
P("MC2_LOWCAM_OBJ_NEARPAD_SCALE", Float, 4.0, "code", "", "[LOW-CAMERA-OBJECT-CULL-1 v2] getExtentRadius() is a tight extent; near objects still pop at low pitch."),
L(Lead0Kill, "MC2_LOWCAM_PICK", Bool, true, "code", "", "[LOW-CAMERA-PICK-RAY-1 v2] Two root causes make the legacy picker mis-land at LOW/grazing pitch (greybeard + adversarial recon, both in the EXISTING..."),
P("MC2_LOWCAM_SOLID_NEAR", Float, 6.0, "terrain", "", "[LOW-CAMERA-TERRAIN-CULL-1 v2] The angular solid-window producer (this loop) is the SECOND terrain visibility gate - independent of the frustum inFrustum..."),
L(Lead0Kill, "MC2_LOWCAM_TERRAIN_NEAR", Bool, true, "terrain", "", "[LOW-CAMERA-TERRAIN-CULL-1 / FIX-2] When set, drop the near plane from the terrain AABB frustum test so near terrain isn't culled at a low/grazing pitch..."),
L(Lead0, "MC2_LOWCAM_ZOOM_ANCHOR", Bool, false, "code", "", "Default OFF: anchored zoom still reads as a pan in practice; use the legacy fixed-pivot wheel zoom."),
L(Presence, "MC2_M2D_PZ_PARITY", Bool, false, "terrain", "", "MC2_M2D_PZ_PARITY probe (VPL Step 8b precursor validator)."),
// history: docs/gate-history.md#MC2_MATERIAL_GPU
L(Lead0Kill, "MC2_MATERIAL_GPU", Bool, true, "unclassified", "", "GPU material table upload/bind/compare for static props (default-ON as of v5, 2026-05-26).", passthrough),
// history: docs/gate-history.md#MC2_MATERIAL_GPU_SAMPLE
L(Lead0Kill, "MC2_MATERIAL_GPU_SAMPLE", Bool, true, "props", "", "GPU material albedo sampling in static_prop.frag (default-ON as of v7, 2026-05-26).", passthrough),
// history: docs/gate-history.md#MC2_MATERIAL_KTX
L(Lead0P, "MC2_MATERIAL_KTX", Bool, false, "props", "", "KTX2 sidecar loader for static-prop textures (RenderCore/KtxLoader)."),
L(Presence, "MC2_MDI_SUBMIT_TRACE", Bool, false, "gameos", "", "", passthrough),
L(Lead0Kill, "MC2_MECH_AMBIENT_V1", Bool, true, "mech", "", ""),
P("MC2_MECH_AMBIENT_V1_STRENGTH", Float, 0.15, "mech", "", "", range(0.0, 2.0)),
L(Lead0Kill, "MC2_MECH_ANIM_ROTATION_ONLY", Bool, true, "mech", "", ""),
P("MC2_MECH_BACK_FILL", Float, 0.0, "mech", "", ""),
L(Presence, "MC2_MECH_BATCHER_STATS", Bool, false, "mech", "", ""),
P("MC2_MECH_FRAG_DEBUG", Int, 0, "mech", "", "MC2_MECH_FRAG_DEBUG=N: legacy env path; wins over ImGui s_mechDebugMode."),  // default not derivable from the tree
P("MC2_MECH_GLASS_LUMA_THRESH", Float, 0.12, "mech", "", "", range(0.0, 1.0)),
P("MC2_MECH_GLASS_MAXCHAN_THRESH", Float, 0.18, "mech", "", "", range(0.0, 1.0)),
P("MC2_MECH_GLASS_ROUGHNESS", Float, 0.25, "mech", "", "", range(0.04, 1.0)),
L(Lead0Kill, "MC2_MECH_IBL_SH", Bool, true, "mech", "", ""),
L(Lead1, "MC2_MECH_IMPORT_ANIMATE", Bool, false, "code", "", "Opt-in, default OFF (static FORCE_CLIP bake stays the safe path)."),
L(Presence, "MC2_MECH_IMPORT_CLIP_DIAG", Bool, false, "mech", "", ""),
L(Lead1, "MC2_MECH_IMPORT_DYNAMIC_LIFT", Bool, false, "code", "", "Default: static rest lift (steady torso)."),
G("MC2_MECH_IMPORT_FORCE_BONE", String, "", "mech", "", ""),  // default not derivable from the tree
G("MC2_MECH_IMPORT_FORCE_CLIP", Bool, false, "code", "", "BT2018-SKEL-ENGINE-1B: by default bake the REST (bind) pose."),  // default not derivable from the tree
P("MC2_MECH_IMPORT_FORCE_FRAME", Int, 0, "code", "", ""),
P("MC2_MECH_IMPORT_FORCE_POSE", Int, 0, "mech", "", ""),
L(Lead1, "MC2_MECH_IMPORT_GPU", Bool, false, "code", "", ""),
P("MC2_MECH_IMPORT_GPU_AXIS", Int, 2, "code", "", ""),
P("MC2_MECH_IMPORT_GPU_LIFT", Float, 0.0, "code", "", ""),
P("MC2_MECH_IMPORT_GPU_LIFT_AXIS", Int, 1, "code", "", ""),
// history: docs/gate-history.md#MC2_MECH_LIGHT_FRESHNESS
L(Lead0Kill, "MC2_MECH_LIGHT_FRESHNESS", Bool, true, "mech", "", "MECH-LIGHTROW-FRESHNESS-1, amended by MECH-DECLINE-FIX-1: DEFAULT-ON.", passthrough),
L(Presence, "MC2_MECH_LIGHT_TRACE", Bool, false, "mech", "", ""),
L(Presence, "MC2_MECH_LOD_TRACE", Bool, false, "mech", "", "MC2_MECH_LOD_TRACE=1: per-actor LOD-swap boundary print."),
P("MC2_MECH_METAL_ROUGHNESS", Float, 0.85, "mech", "", "", range(0.04, 1.0)),
L(Presence, "MC2_MECH_NODE_DIAG", Bool, false, "mech", "", ""),
L(Presence, "MC2_MECH_NODE_TRACE", Bool, false, "mech", "", ""),
P("MC2_MECH_NORMALS_MODE", Int, 2, "mech", "", ""),
P("MC2_MECH_NORMALS_SMOOTH_DEG", Float, 60.0, "mech", "", "", range(1.0, 179.0)),
L(Lead0, "MC2_MECH_OBJECT_ID_SELFTEST", Bool, false, "code", "", ""),
L(Lead0, "MC2_MECH_PICK", Bool, false, "code", "", ""),
L(Lead0, "MC2_MECH_PICK_DEBUG", Bool, false, "code", "", ""),
L(Lead0, "MC2_MECH_PICK_PIERCE_FOG", Bool, false, "code", "", ""),
L(Lead0, "MC2_MECH_PICK_SELFTEST", Bool, false, "mech", "", ""),
L(Presence, "MC2_MECH_PREVIEW_TRACE", Bool, false, "mech", "", ""),
L(Presence, "MC2_MECH_RESTORE_TRACE", Bool, false, "mech", "", "", passthrough),
L(Presence, "MC2_MECH_SKEL_BONE_DUMP", Bool, false, "code", "", "MECH-BONE-PARITY-GATE-1: dump the engine's REST joint-global bone matrices (the same mc2skel output the bake consumes) as JSON when..."),
P("MC2_MECH_SKEL_HEIGHT", Float, 50.0, "code", "", ""),
L(Presence, "MC2_MECH_SKEL_TRACE", Bool, false, "code", "", "BT2018-SKEL-ENGINE-1A - bind-pose bake trace (default OFF)."),
P("MC2_MECH_SPECULAR_STRENGTH", Float, 0.05, "mech", "", "", range(0.0, 4.0)),
L(Lead0Kill, "MC2_MECH_SPECULAR_V1", Bool, true, "mech", "", ""),
G("MC2_MECH_SURFACE_MATERIAL", Enum, "", "mech", "", "Gate: MC2_MECH_SURFACE_MATERIAL controls which profiles to load.", tokens("|0|metal061b|none|painted_subtle")),
L(Presence, "MC2_MECH_TEX_READBACK", Bool, false, "mech", "", "Pixel-readback diagnostic - gated on its own env var (NOT MC2_MECH_BATCHER_STATS) because glGetTexImage is a synchronous GPU stall that pollutes any perf..."),
L(Lead0Kill, "MC2_MECH_VIEWUNIFORMS", Bool, true, "mech", "", ""),
L(Lead1, "MC2_MECH_VIEWUNIFORMS_DIAG", Bool, false, "mech", "", ""),
L(Lead0, "MC2_MENU_CANARY_SKIP_INTRO", Bool, false, "code", "", "", passthrough),
L(Presence, "MC2_MIF_SPLIT", Bool, false, "code", "", "", passthrough),
L(Lead0Kill, "MC2_MISSING_RUNWAY_FIX", Bool, true, "terrain", "", ""),
L(Presence, "MC2_MISSION_CYCLE_TEST", Bool, false, "code", "", "", passthrough),
// history: docs/gate-history.md#MC2_MISSION_INTRO_LEGACY_RENDER
G("MC2_MISSION_INTRO_LEGACY_RENDER", Bool, false, "terrain", "", "TRACKV-CI-GATE-RESTORE-1: mission intro render path kill-switch.", passthrough),
L(Presence, "MC2_MISSION_SPLIT", Bool, false, "code", "", "", passthrough),
L(Lead0Kill, "MC2_MODERN_TERRAIN_PATCHES", Bool, true, "terrain", "", "Default ON post Slice-1 flip (2026-04-29).", passthrough),
L(Lead0Kill, "MC2_MODERN_TERRAIN_SURFACE", Bool, true, "terrain", "", "", passthrough),
G("MC2_MODERN_TEX_RESOLVE", Bool, true, "code", "", ""),
L(Presence, "MC2_MODERN_TEX_RESOLVE_TRACE", Bool, false, "code", "", ""),
L(Presence, "MC2_MODERN_TEX_RESOLVE_VALIDATE", Bool, false, "code", "", "Default ON with explicit opt-out."),
L(Presence, "MC2_MODOVERRIDE_TRACE", Bool, false, "props", "", ""),
G("MC2_MOD_DEPS", String, "", "code", "", "Stock campaign: no active mod.", passthrough),
L(Presence, "MC2_MOUSE_RECON", Bool, false, "unclassified", "", "MC2_MOUSE_RECON: env-gated one-shot-ish confirm log."),
// history: docs/gate-history.md#MC2_MOVER_COHERENCE_AUDIT
L(Lead0, "MC2_MOVER_COHERENCE_AUDIT", Bool, false, "code", "", "META-FIX design A runtime tier (code/mission.cpp auditMoverCoherence, ~1 Hz): read-only audit that the five per-mover registries (isOnGUI render gate...", passthrough),
P("MC2_MOVER_CONTACT_SHADOW", Float, 0.0, "terrain", "", "", passthrough),
L(Lead0, "MC2_MOVE_CHUNK_SHADOW", Bool, false, "code", "", "", passthrough),
L(Lead0, "MC2_MOVE_PATH_CACHE_SHADOW", Bool, false, "code", "", "", passthrough),
// history: docs/gate-history.md#MC2_MOVE_RECON
L(Lead0, "MC2_MOVE_RECON", Bool, false, "code", "", "Pathfinding cost instrumentation (mclib/move_recon.cpp).", passthrough),
L(DefOnAtoiNZ, "MC2_MVP_PUBLISH_EARLY", Bool, true, "code", "", ""),  // default not derivable from the tree
// history: docs/gate-history.md#MC2_NIGHT_AMBIENT_FLOOR
P("MC2_NIGHT_AMBIENT_FLOOR", Int, 0, "code", "", "NIGHT-LIGHT-POOLS-1 knob: minimum per-channel output (0..64, clamped) of Camera::getLightRed/Green/Blue -- the family feeding the colormap night burn-in...", range(0, 64)),
// history: docs/gate-history.md#MC2_NIGHT_FOG_DIM
L(Exact1, "MC2_NIGHT_FOG_DIM", Bool, false, "post", "", "BACKDROP-BLIT-ATTRIB-1: RenderDoc pixel-history on the pre-composite HDR scene FBO (231) pinned the mc2_10 night 'white horizon band' to the EDGE-FOG...", passthrough),
// history: docs/gate-history.md#MC2_NIGHT_FOG_DIM_SCALE
P("MC2_NIGHT_FOG_DIM_SCALE", Float, 0.0, "post", "", "BACKDROP-BLIT-ATTRIB-1: multiplier applied per-channel to the edge-fog + OOB fog color when MC2_NIGHT_FOG_DIM=1.", passthrough),  // default not derivable from the tree
// history: docs/gate-history.md#MC2_NO_ACTIVATE
L(Lead0, "MC2_NO_ACTIVATE", Bool, false, "editor", "", "Mission Editor (editor/EditorMFC.cpp, MainFrm.cpp): come up minimized and non-activating, never taking the foreground or keyboard focus."),
// history: docs/gate-history.md#MC2_NO_CURSOR_GRAB
L(Lead0Def, "MC2_NO_CURSOR_GRAB", Bool, false, "gameos", "", "SMOKE-NO-CURSOR-STEAL-1: suppress ALL OS-cursor manipulation -- window mouse-grab, Win32 ClipCursor clamp, and gos_SetMousePosition warps -- plus window..."),
L(Exact1, "MC2_NO_CURSOR_GUARD", Bool, false, "harness", "", "smoke harness only (cursor_guard.py): do not start the cursor guard"),
L(Presence, "MC2_NO_LAUNCHER", Bool, false, "gameos", "", "", passthrough),
L(Presence, "MC2_OBJBATCHER_TRACE", Bool, false, "props", "", ""),
G("MC2_OBJECT_ADMISSION_PREDICATE", Enum, "", "code", "", "", passthrough | tokens("|legacy")),
L(Exact1, "MC2_OBJECT_ADMISSION_SELFTEST", Bool, false, "gameos", "", "Optional startup selftest - hard-fails on any boundary violation so the operator knows immediately if the predicate body has a regression."),
// history: docs/gate-history.md#MC2_OBJECT_ID_BUFFER
L(DefOnLead0, "MC2_OBJECT_ID_BUFFER", Bool, true, "code", "", "R32_UINT MRT attachment-2 with per-pixel object handles (RenderWorld M1.5).", passthrough),
L(Lead0, "MC2_OBJECT_ID_BUFFER_SELFTEST", Bool, false, "code", "", ""),
L(Exact1, "MC2_OBJECT_PARITY_CHECK", Bool, false, "gameos", "", ""),
L(Exact1, "MC2_OBJECT_PARITY_TRACE", Bool, false, "code", "", ""),
L(Lead0P, "MC2_OBJECT_POLY_OFFSET", Bool, false, "props", "", "", passthrough),
P("MC2_OBJECT_POLY_OFFSET_FACTOR", Float, -1.0, "props", "", "", passthrough),
P("MC2_OBJECT_POLY_OFFSET_UNITS", Float, -1.0, "props", "", "", passthrough),
L(Lead0, "MC2_OBJECT_RECON_TRACY", Bool, false, "unclassified", "", "", passthrough),
L(Lead0, "MC2_OBJECT_WALK_FACTS", Bool, false, "code", "", ""),
L(Presence, "MC2_OBJ_MVP_STALE_FATAL", Bool, false, "gameos", "", ""),
L(Lead0Kill, "MC2_OOB_FOG", Bool, true, "post", "", ""),
L(Presence, "MC2_OOB_FOG_COLOR", Bool, false, "post", "", ""),
L(Lead0Kill, "MC2_OOB_LETTERBOX", Bool, true, "post", "", ""),
L(Presence, "MC2_OS_CURSOR", Bool, false, "unclassified", "", "Hide the OS cursor."),
// history: docs/gate-history.md#MC2_OVERLAY_ART_FLATLIGHT
L(Lead0Kill, "MC2_OVERLAY_ART_FLATLIGHT", Bool, true, "gameos", "", "RUNWAY-CLOSE-1: A/B arm for the REOPENED 'runway is the wrong color' item (user re-grade 2026-08-02: 'there is no difference here to me').", passthrough),
L(Lead0, "MC2_OVERLAY_BIAS_LEGACY", Bool, false, "gameos", "", ""),
// history: docs/gate-history.md#MC2_OVERLAY_CEMENT_ADOPT
L(Lead0Kill, "MC2_OVERLAY_CEMENT_ADOPT", Bool, true, "terrain", "", "OVERLAY-CEMENT-ADOPT-1: closes the two-shader concrete seam CEMENT-PATCHY-CAUSE-1 attributed but explicitly left open as a design call ('either the overlay...", passthrough),
// history: docs/gate-history.md#MC2_OVERLAY_CEMENT_SHADOW_MATCH
L(Lead0, "MC2_OVERLAY_CEMENT_SHADOW_MATCH", Bool, false, "gameos", "", "CEMENT-PATCHY-CAUSE-1: make terrain_overlay.frag apply the SAME concrete lighting exemption terrain_lod_chunk.frag already applies, so the overlay/decal...", passthrough),
L(Presence, "MC2_OVERLAY_MAGENTA_TRACE", Bool, false, "gameos", "", ""),
L(Presence, "MC2_OVERLAY_TEXTURE_TRACE", Bool, false, "gameos", "", "", passthrough),
// history: docs/gate-history.md#MC2_OVERLAY_TILE_HIRES
P("MC2_OVERLAY_TILE_HIRES", Int, 0, "terrain", "", "OVERLAY-TILE-HIRES-1: honor the overlay tile TGA's actual dimensions instead of the legacy hard-assumed 64px (initOverlay header resync mirrors...", passthrough),  // default not derivable from the tree
P("MC2_PAD_AREA_GLOW", Float, 0.12, "terrain", "", "", passthrough | range(0.0, 4.0)),
P("MC2_PAD_AREA_GLOW_SPILL", Float, 48.0, "terrain", "", "", passthrough | range(4.0, 127.0)),
P("MC2_PAD_EDGE_ROAD_FIX", Int, 1, "terrain", "", ""),
L(Presence, "MC2_PARTICLE_FLUSH_STALL_TRACE", Bool, false, "gameos", "", ""),
L(Presence, "MC2_PATCHSTREAM_DIRECT_TEXTURE_BIND", Bool, false, "terrain", "", ""),
L(Presence, "MC2_PATCHSTREAM_DIRECT_TEXTURE_BIND_CHECK", Bool, false, "terrain", "", ""),
L(Presence, "MC2_PATCHSTREAM_QUAD_RECORDS", Bool, false, "terrain", "", "", passthrough),
L(Presence, "MC2_PATCHSTREAM_QUAD_RECORDS_DRAW", Bool, false, "terrain", "", "", passthrough),
L(Presence, "MC2_PATCHSTREAM_THIN_RECORDS", Bool, false, "terrain", "", "", passthrough),
L(Presence, "MC2_PATCHSTREAM_THIN_RECORDS_DRAW", Bool, false, "terrain", "", "", passthrough),
L(Presence, "MC2_PATCHSTREAM_THIN_RECORD_FASTPATH", Bool, false, "terrain", "", "", passthrough),
G("MC2_PATCH_STREAM_FORCE_INIT_FAIL", Bool, true, "terrain", "", "Step 4 bailout - env-gated forced init failure for testing the fallback path without driver stress.", passthrough),
L(Presence, "MC2_PATCH_STREAM_TRACE", Bool, false, "terrain", "", "", passthrough),
L(Presence, "MC2_PATH_JUMP_FAIL_BACKOFF", Bool, false, "code", "", "PATHFINDING-JUMP-FAIL-1: backoff gate + constants.", passthrough),
L(Lead1, "MC2_PATH_SOLVE_ISOLATED", Bool, false, "code", "", "", passthrough),
// history: docs/gate-history.md#MC2_PAVED_OVERLAY_CLASS
P("MC2_PAVED_OVERLAY_CLASS", Int, 0, "terrain", "", "DEFAULT 0 (OFF).", passthrough),
P("MC2_PBR_AMBIENT_SPECULAR", Float, 0.25, "mech", "", ""),
P("MC2_PBR_METALLIC_INFLUENCE", Float, 0.0, "mech", "", ""),  // default not derivable from the tree
P("MC2_PBR_ROUGHNESS_MAX", Float, 0.9, "mech", "", ""),
P("MC2_PBR_ROUGHNESS_MIN", Float, 0.0, "mech", "", ""),  // default not derivable from the tree
P("MC2_PBR_TILE_SCALE", Float, 0.0, "mech", "", ""),  // default not derivable from the tree
L(Lead1, "MC2_PBR_TRIPLANAR", Bool, false, "mech", "", ""),
P("MC2_PBR_TRIPLANAR_SCALE", Float, 0.2, "mech", "", ""),
P("MC2_PBR_WEAR_STRENGTH", Float, 0.0, "mech", "", ""),  // default not derivable from the tree
L(Presence, "MC2_PICK_BEHIND_LEGACY", Bool, false, "code", "", "INPUT-CURSOR-OFFSCREEN-TARGET-RECON-1 / -FIX-1: a near-plane-clipped OBB corner projects to (0,0) with w<=1e-4 (camera.h:860-862)."),
L(Presence, "MC2_PICK_CAP_TRACE", Bool, false, "code", "", "", passthrough),
// history: docs/gate-history.md#MC2_PICK_FALLBACK_COARSE
L(Presence, "MC2_PICK_FALLBACK_COARSE", Bool, false, "code", "", "MISSION-INTERFACE-PERF-1: coarse-to-fine closest-vertex fallback in Camera::inverseProject (mclib/camera.cpp).", passthrough),
L(Presence, "MC2_PICK_RECON", Bool, false, "code", "", "", passthrough),
L(Lead0, "MC2_PIPELINE_BIND_TRACE", Bool, false, "gameos", "", "", passthrough),
L(Lead0, "MC2_PIPELINE_COLORMASK", Bool, false, "gameos", "", ""),
P("MC2_PLAYTEST_EXE", Int, 0, "editor", "", ""),
L(Lead0Kill, "MC2_PNG_MAGENTA_KEY", Bool, true, "gameos", "", ""),
G("MC2_POSTPROCESS_BACKEND", Enum, "", "post", "", "", passthrough | tokens("|v")),
L(Lead0P, "MC2_POSTPROCESS_COMPUTE_BLUR", Bool, false, "post", "", ""),
L(Lead0P, "MC2_POSTPROCESS_COMPUTE_BLUR_PLANT", Bool, false, "post", "", ""),
L(Lead0P, "MC2_POSTPROCESS_COMPUTE_BLUR_VERIFY", Bool, false, "post", "", ""),
// history: docs/gate-history.md#MC2_POST_CAS
L(Lead0, "MC2_POST_CAS", Bool, false, "post", "", "POST-FX-CAS-1: gated AMD FidelityFX CAS sharpening folded into the final composite shader (after tonemap/grade, before UI)."),
// history: docs/gate-history.md#MC2_POST_CAS_SHARPNESS
P("MC2_POST_CAS_SHARPNESS", Float, 0.0, "post", "", "POST-FX-CAS-1: CAS sharpness knob, clamped 0..1 (default 0.5).", range(0.0, 1.0)),  // default not derivable from the tree
L(Presence, "MC2_PRESWAP_FINISH", Bool, false, "gameos", "", "", passthrough),
P("MC2_PREVIEW_FBO_SCALE", Int, 0, "gameos", "", ""),  // default not derivable from the tree
L(Presence, "MC2_PREVIEW_NOCULL", Bool, false, "code", "", "EXPERIMENT (PREVIEW-FBO-FIXED-800x600-1 debug): the mech draws zero pixels anywhere in the FBO despite shouldRender=1."),
L(Lead1, "MC2_PREVIEW_SCENE_DRAIN", Bool, false, "code", "", ""),
L(Lead0, "MC2_PROJECTED_DECALS", Bool, false, "post", "", "DECAL-INTEGRATE-1: MC2_PROJECTED_DECALS (default OFF)."),
// history: docs/gate-history.md#MC2_PROJECTILE_SPEED_MULT
P("MC2_PROJECTILE_SPEED_MULT", Float, 1.0, "code", "", "Direct-fire projectile speed multiplier (PPC/autocannon/gauss = non-arc bolts).", passthrough),
G("MC2_PROJECTZ_BYPASS_MODE", Enum, "", "code", "", "", tokens("|Compare|Off")),
P("MC2_PROJECTZ_GUARD_PX", Int, 0, "code", "", "", passthrough),  // default not derivable from the tree
L(Presence, "MC2_PROJECTZ_HEATMAP", Bool, false, "code", "", "", passthrough),
L(Presence, "MC2_PROJECTZ_SUMMARY", Bool, false, "code", "", "", passthrough),
L(Presence, "MC2_PROJECTZ_TRACE", Bool, false, "code", "", "", passthrough),
// history: docs/gate-history.md#MC2_PROP_BASE_LIGHT_DIR
P("MC2_PROP_BASE_LIGHT_DIR", Float, 0.0, "props", "", "PROP-BASE-LIGHT-DIR-1: give the static-prop baked per-vertex base_light seed a directional (hemispherical-wrap) orientation term, so faces pointing away...", passthrough | range(0.0, 1.0)),
L(DefOnAtoiNZ, "MC2_PROP_FIXB_MVP", Bool, true, "props", "", ""),  // default not derivable from the tree
// history: docs/gate-history.md#MC2_PROP_FOLIAGE_ALPHAFLAG
L(Lead0, "MC2_PROP_FOLIAGE_ALPHAFLAG", Bool, false, "props", "", "FOLIAGE-ALPHAFLAG-1: make the per-instance flags bit 2 emitted by the static-prop RECIPE paths (BldgAppearance/TreeAppearance::registerStatic...", passthrough),
// history: docs/gate-history.md#MC2_PROP_LIGHT_LATCH_DIAG
L(Lead0, "MC2_PROP_LIGHT_LATCH_DIAG", Bool, false, "props", "", "FOGLIGHT-LATCH-1: dump per-static-prop-actor [PROPLATCH v1] lines -- latch frame + latched lightRGB (the fogLightSet one-shot sample fed to the SUN slot) vs...", passthrough),
// history: docs/gate-history.md#MC2_PROP_LIGHT_LATCH_DIAG_INTERVAL
P("MC2_PROP_LIGHT_LATCH_DIAG_INTERVAL", Int, 120, "props", "", "FOGLIGHT-LATCH-1: frame throttle for the [PROPLATCH v1] compare line (default 120).", passthrough),
// history: docs/gate-history.md#MC2_PROP_SHADOW_RECEIVE
P("MC2_PROP_SHADOW_RECEIVE", Int, 0, "post", "", "PROP-SHADOW-RECEIVE-1: static props RECEIVE the dynamic CSM cascade shadow in shadow_screen.frag (building self-shadow + prop-on-prop; the cascades already...", passthrough),
L(Presence, "MC2_PURCHASE_ALL", Bool, false, "code", "", "MC2_PURCHASE_ALL (default OFF; launcher Engine Option): unlock every COMPONENT/weapon so modded gear (e.g."),
L(Lead0Kill, "MC2_QUADSETUP_ARMED_SKIP", Bool, true, "terrain", "", "", passthrough),
L(Presence, "MC2_R2B_STATIC_NATURAL_TRACE", Bool, false, "code", "", "", passthrough),
L(Lead0Kill, "MC2_R2B_TOUCH_PRESERVE", Bool, true, "code", "", "", passthrough),
L(Lead0Kill, "MC2_RAIN_BATCH", Bool, true, "code", "", ""),
P("MC2_RDC_CAPTURE_FRAME", Int, 0, "gameos", "", ""),
L(NonEmpty, "MC2_RDC_CAPTURE_PATH", Bool, false, "gameos", "", "Expand and VALIDATE the path template here, at arm time -- deliberately NOT down in resolveApiOnce()."),
L(Presence, "MC2_RDC_EXIT_AFTER", Bool, false, "gameos", "", ""),
G("MC2_RDC_MAX_WAIT", Bool, false, "gameos", "", ""),  // default not derivable from the tree
L(NonEmpty, "MC2_RDC_MISSION", Bool, false, "gameos", "", ""),
L(Presence, "MC2_REBUILD_MOD_CACHE", Bool, false, "code", "", ""),
L(Presence, "MC2_REGFLUSH_DIAG_TRACE", Bool, false, "props", "", "", passthrough),
L(Presence, "MC2_REGFLUSH_GUARD2", Bool, false, "props", "", "STATIC-REGISTRY-CURRENTNESS-GUARD-2: generalize the R2b stale_after_drawn counter beyond the single tree case.", passthrough),
L(Presence, "MC2_REGFLUSH_MULTI", Bool, false, "props", "", ""),
L(Presence, "MC2_REGFLUSH_TYPEHIST", Bool, false, "props", "", ""),
// history: docs/gate-history.md#MC2_RENDERLISTS_COST_SPLIT
L(Lead0P, "MC2_RENDERLISTS_COST_SPLIT", Bool, false, "code", "", "TXMMGR-PERF-EASYWINS-1: coarse per-phase CPU cost split of MC_TextureManager::renderLists() (mclib/txmmgr.cpp).", passthrough),
L(Exact1, "MC2_RENDERSTATES_LEGACY", Bool, false, "gameos", "", ""),
L(Presence, "MC2_RENDERSTATES_TRACE", Bool, false, "gameos", "", "Optional diagnostic: print the first 8 mismatch reasons."),
// history: docs/gate-history.md#MC2_RENDER_BACKEND
L(NonEmpty, "MC2_RENDER_BACKEND", Bool, false, "unclassified", "", "VK-P13-SELECTOR-1: top-level render-backend selector, values gl|vulkan (default gl; unset/unrecognized -> gl, coercion logged).", passthrough),
// history: docs/gate-history.md#MC2_RENDER_BACKEND_IFACE
L(Lead1, "MC2_RENDER_BACKEND_IFACE", Bool, false, "post", "", "RENDER-BACKEND-SEAMS arc: routes select GL calls (e.g.", passthrough),
L(Lead1, "MC2_RENDER_BACKEND_REGION_IFACE", Bool, false, "post", "", "", passthrough),
L(Lead0, "MC2_RENDER_CONTRACT_ASSERT", Bool, false, "code", "", "", passthrough),
L(Presence, "MC2_RENDER_FRAME_DRIVER", Bool, false, "gameos", "", "GAME-EDITOR-RENDER-FRAME-DRIVER-1 (Slice 6): gated A/B adoption of the shared RenderFrameDriver seam."),
L(Presence, "MC2_RENDER_FRAME_PLAN_TRACE", Bool, false, "gameos", "", "", passthrough),
L(Lead1, "MC2_RENDER_PASS_CONTRACT_ASSERT", Bool, false, "code", "", ""),
L(Lead1, "MC2_RENDER_PASS_CONTRACT_TRACE", Bool, false, "code", "", ""),
// history: docs/gate-history.md#MC2_RENDER_PASS_ORDER
L(Lead1, "MC2_RENDER_PASS_ORDER", Bool, false, "code", "", "CONTRACT-3: per-frame resource-ordering audit (mclib/render_contract.cpp) - checks each pass's reads[] are satisfied by a prior write before beginPass."),
L(Lead1, "MC2_RENDER_PASS_TELEMETRY", Bool, false, "code", "", "", passthrough),
L(Exact1, "MC2_RENDER_PASS_TIME", Bool, false, "gameos", "", "", passthrough),
P("MC2_RENDER_PASS_TIME_EVERY", Int, 0, "gameos", "", "", passthrough),
L(Presence, "MC2_RENDER_PATH", Bool, false, "props", "", "G5 RENDER-PATH OBSERVABILITY (descriptive; env-gated; ZERO behavior change): one line per registered static-prop type recording which render path it takes."),
L(Lead1, "MC2_RENDER_SNAPSHOT_LOG", Bool, false, "gameos", "", "", passthrough),
L(Presence, "MC2_RENDER_WATER_FASTPATH", Bool, false, "terrain", "", "", passthrough),
P("MC2_RENDER_WATER_FASTPATH_DEBUG", Int, 0, "gameos", "", ""),
L(Presence, "MC2_RENDER_WATER_PARITY_CHECK", Bool, false, "terrain", "", "", passthrough),
L(Lead0, "MC2_RENDER_WORLD_SELFTEST", Bool, false, "code", "", ""),
// history: docs/gate-history.md#MC2_RENDER_WORLD_TRACE
L(Lead0, "MC2_RENDER_WORLD_TRACE", Bool, false, "unclassified", "", "Per-frame [RENDER_WORLD v1] banner + per-event logs (RenderWorld.cpp)."),
// history: docs/gate-history.md#MC2_RESOLVE_TRACE_FILE
L(NonEmpty, "MC2_RESOLVE_TRACE_FILE", Bool, false, "code", "", "META-FIX design E tier 3 (mclib/file.cpp): output path for the MC2_LOG_FILE_RESOLVE=2 JSONL resolve trace, opened for append at InitModSearchPaths (relative...", passthrough),
// history: docs/gate-history.md#MC2_RES_ASSERT
L(Lead0, "MC2_RES_ASSERT", Bool, false, "code", "", "META-FIX design D runtime tier (RenderCore/ResolutionContract.h): resolution/aspect contract audit.", passthrough),
L(Presence, "MC2_RES_DIAG", Bool, false, "post", "", "[RES_DIAG v1] One-shot dump of the resolution split: scene FBO size (this object's width_/height_) vs the HUD canvas (Environment.screenWidth) vs the native...", passthrough),
L(Presence, "MC2_REVERSE_Z_TRACE", Bool, false, "unclassified", "", "", passthrough),
L(Presence, "MC2_RING_FORCE_FINISH", Bool, false, "terrain", "", "?? Probe 7 (MC2_RING_FORCE_FINISH) - directed barrier-hypothesis test ?? Forces full GPU pipeline drain after the compute dispatches.", passthrough),
L(Presence, "MC2_RING_TRACE", Bool, false, "terrain", "", "?? Probe 8: compare draw-time MVP fingerprint vs compute-time fingerprint.", passthrough),
P("MC2_RNG_SEED", Int, 0, "mech", "", "", passthrough),  // default not derivable from the tree
// history: docs/gate-history.md#MC2_RUNWAY_EDGE_BLEND
L(Lead0, "MC2_RUNWAY_EDGE_BLEND", Bool, false, "terrain", "", "RUNWAY-EDGE-BLEND-1 (MASTER-FAIL #6), REVISION 2 = DILATION: cure the hard full-tile stairstep cliff where a SOLID cement pad/runway tile abuts BARE terrain...", passthrough),
L(Presence, "MC2_SCENE_LIGHTING_ASSERT", Bool, false, "code", "", ""),
L(Presence, "MC2_SCENE_LIGHTING_TRACE", Bool, false, "code", "", ""),
P("MC2_SCREENSHOT_AT_FRAME", Int, -1, "gameos", "", "", passthrough),
L(Presence, "MC2_SCREENSHOT_PATH", Bool, false, "gameos", "", "Oracle: deterministic backbuffer screenshot at a target frame.", passthrough),
G("MC2_SCREENSHOT_SOURCE", Enum, "", "gameos", "", "", tokens("|backbuffer")),
G("MC2_SCREENXY_PREDICATE_MODE", Enum, "", "code", "", "", tokens("|Legacy")),
G("MC2_SELECTION_PICKING_PREDICATE_MODE", Enum, "", "code", "", "", tokens("|Legacy")),
L(Lead0, "MC2_SENSOR_SCAN_FACTS", Bool, false, "code", "", ""),
// history: docs/gate-history.md#MC2_SETUPTEXTURES_LEGACY_FORCE
L(Presence, "MC2_SETUPTEXTURES_LEGACY_FORCE", Bool, false, "terrain", "", "TRACKV-CI-GATE-RESTORE-1: setupTextures legacy path kill-switch."),
L(Lead1, "MC2_SHADER_HOT_RELOAD", Bool, false, "gameos", "", ""),
P("MC2_SHADER_PATH_TINT", Int, 0, "post", "", ""),
// history: docs/gate-history.md#MC2_SHADER_SPIRV
L(Lead1, "MC2_SHADER_SPIRV", Bool, false, "gameos", "", "SPIRV-CONSUMER-PILOT-BUILD-1: DEFAULT-OFF. =1 loads the offline-baked OpenGL SPIR-V (OFFLINE-SHADER-VARIANT-BUILD-1, shaders/spv/) for the postprocess..."),
// history: docs/gate-history.md#MC2_SHADER_SPIRV_FATAL
L(Lead1, "MC2_SHADER_SPIRV_FATAL", Bool, false, "gameos", "", "SPIRV-CONSUMER-PILOT-BUILD-1: DEFAULT-OFF debug gate."),
L(Lead0Kill, "MC2_SHADOW_ALT_SCALE", Bool, true, "post", "", "", passthrough),
// history: docs/gate-history.md#MC2_SHADOW_BOUNDED_NEAR_FIT
L(Lead0Kill, "MC2_SHADOW_BOUNDED_NEAR_FIT", Bool, true, "post", "", "SHADOW-BOUNDED-NEAR-FIT-1: cap the dynamic sun-shadow frustum fit radius to a small camera-centered region (radius from MC2_SHADOW_BOUNDED_NEAR_RADIUS...", passthrough),
// history: docs/gate-history.md#MC2_SHADOW_BOUNDED_NEAR_RADIUS
P("MC2_SHADOW_BOUNDED_NEAR_RADIUS", Float, 2500.0, "post", "", "SHADOW-BOUNDED-NEAR-FIT-1 tunable: bounded near-fit radius in world units (default 2500, clamped 512..mapClampR).", passthrough),
// history: docs/gate-history.md#MC2_SHADOW_CASTER_CULL_CACHE
L(Lead0P, "MC2_SHADOW_CASTER_CULL_CACHE", Bool, false, "code", "", "SHADOW-CASTER-CULL-CACHE-1 (TXMMGR-PERF-EASYWINS-1): reuse the Shadow.CasterCull result (mclib/txmmgr.cpp DynamicShadowPass) across frames when BOTH the...", passthrough),
P("MC2_SHADOW_CASTER_CULL_MARGIN", Float, 0.25, "code", "", "", passthrough),
L(Lead0Kill, "MC2_SHADOW_CASTER_LIGHTBOX_CULL", Bool, true, "code", "", "DEFAULT ON since 2026-06-16 (render-hygiene-s1).", passthrough),
// history: docs/gate-history.md#MC2_SHADOW_CSM
L(Lead0Kill, "MC2_SHADOW_CSM", Bool, true, "post", "", "Cascaded shadow maps master gate (GameOS/gameos/gos_postprocess.cpp), reworked 2026-06-18 (commit 8ff13a36).", passthrough),
P("MC2_SHADOW_CSM_COUNT", Int, 3, "post", "", "", range(1, 3)),
P("MC2_SHADOW_CSM_LAMBDA", Float, 0.5, "post", "", "", range(0.0, 1.0)),
P("MC2_SHADOW_CSM_R0", Float, 512.0, "post", "", ""),
P("MC2_SHADOW_CSM_R1", Float, 4096.0, "post", "", ""),
P("MC2_SHADOW_CSM_SOFTNESS", Float, 0.9, "unclassified", "", "", range(0.1, 6.0)),
L(Presence, "MC2_SHADOW_CULL_DEBUG", Bool, false, "code", "", "", passthrough),
// history: docs/gate-history.md#MC2_SHADOW_DEBUG_MODE
L(Lead1, "MC2_SHADOW_DEBUG_MODE", Bool, false, "post", "", "SHADOW-ENV-DEBUG-MODE-1: selects shadow debug map overlay without ImGui.", passthrough),
L(Presence, "MC2_SHADOW_DIAG", Bool, false, "props", "", ""),
P("MC2_SHADOW_DIST_FADE", Float, 80.0, "post", "", "", passthrough | range(0.0, 100000.0)),
P("MC2_SHADOW_DIST_FADE_MAX", Float, 0.65, "post", "", "", passthrough | range(0.0, 1.0)),
// history: docs/gate-history.md#MC2_SHADOW_DYNAMIC_PROP_CASTERS
L(Lead0Kill, "MC2_SHADOW_DYNAMIC_PROP_CASTERS", Bool, true, "props", "", "SHADOW-DYNAMIC-PROP-CASTERS-1: feed the per-frame DYNAMIC sun-shadow caster pass from the full registry (registered props, visibility-independent) instead...", passthrough),
L(Lead0Kill, "MC2_SHADOW_DYNAMIC_PROP_DIRTY_ONLY", Bool, true, "code", "", "DEFAULT-ON since 2026-06-04 (smoke-clean mc2_24; proven s_registryGeneration dirty-only pattern, shadow pass reads only modelMatrix+typeID): only literal..."),
// history: docs/gate-history.md#MC2_SHADOW_ENABLE
L(Lead0Kill, "MC2_SHADOW_ENABLE", Bool, true, "unclassified", "", "Dynamic object shadow caster pass (GpuStaticPropBatcher + GpuMechBatcher flushShadow).", passthrough),
L(Lead0Kill, "MC2_SHADOW_FADE_CONT", Bool, true, "post", "", "", passthrough),
L(Lead0Kill, "MC2_SHADOW_FADE_RAMP0", Bool, true, "post", "", "", passthrough),
L(Lead0Kill, "MC2_SHADOW_FOCUS_CENTER", Bool, true, "post", "", "", passthrough),
// history: docs/gate-history.md#MC2_SHADOW_FRUSTUM_DIAG
L(Presence, "MC2_SHADOW_FRUSTUM_DIAG", Bool, false, "post", "", "SHADOW-FRUSTUM-AUDIT-1: read-only per-frame dynamic sun-shadow coverage probe in buildDynamicLightMatrix (sun dir, frustum XY, fit/xy radius, map clamp...", passthrough),
L(Lead0Kill, "MC2_SHADOW_FULLMAP_SEPARATE", Bool, true, "post", "", ""),
P("MC2_SHADOW_FULLMAP_SIZE", Int, 4096, "post", "", ""),
// history: docs/gate-history.md#MC2_SHADOW_GUARD_CLAMPED_SUN
L(Lead0Kill, "MC2_SHADOW_GUARD_CLAMPED_SUN", Bool, true, "post", "", "SHADOW-SUNCLAMP-1: feed the shadow_screen.frag back-face guard (`if (NdotL < 0.05) return 1.0;`) the SAME altitude-clamped sun the shadow matrices are built...", passthrough),
P("MC2_SHADOW_MAP_SIZE", Int, 8192, "post", "", ""),
P("MC2_SHADOW_MECH_SOFT", Float, 1.0, "unclassified", "", "", range(0.0, 4.0)),
// history: docs/gate-history.md#MC2_SHADOW_MIN_SUN_ALT
P("MC2_SHADOW_MIN_SUN_ALT", Float, 15.0, "post", "", "SHADOW-SUN-ALT-CLAMP-1 tunable: minimum EFFECTIVE sun altitude in DEGREES used by the shadow projection matrices (mc2ClampShadowSunAltitude...", passthrough),
P("MC2_SHADOW_OBJ_NORMAL_BIAS", Float, 2.0, "unclassified", "", "", range(0.0, 20.0)),
L(Lead0Kill, "MC2_SHADOW_PROP_ALPHA", Bool, true, "props", "", "", passthrough),
L(Lead0Kill, "MC2_SHADOW_PROP_ALPHA_ATLAS", Bool, true, "props", "", "", passthrough),
L(Lead0Kill, "MC2_SHADOW_ROBUST_BASIS", Bool, true, "post", "", ""),
L(Lead0, "MC2_SHADOW_STATE_TRACE", Bool, false, "gameos", "", "", passthrough),
L(Lead0Kill, "MC2_SHADOW_STATIC_BANDFIX", Bool, true, "gameos", "", "SHADOW-BANDING-DEFAULT-ON (2026-07-18): field bug report confirmed the banding persists in the shipped default config (\"this was never fixed\") - the fix..."),
L(Lead0Kill, "MC2_SHADOW_STATIC_BANDFIX_V2", Bool, true, "gameos", "", "SHADOW-BANDING-DEFAULT-ON (2026-07-18): field bug report (banding in and around mech shadows on terrain) verified A/B in-engine: default config shows..."),
L(Presence, "MC2_SHADOW_TIER_STATS", Bool, false, "terrain", "", ""),
L(Presence, "MC2_SHAPE_C_PARITY_CHECK", Bool, false, "terrain", "", "", passthrough),
// history: docs/gate-history.md#MC2_SHORELINE_LEGACY_SUPPRESS
L(Lead0Kill, "MC2_SHORELINE_LEGACY_SUPPRESS", Bool, true, "terrain", "", "[DEFAULT FLIPPED OFF -> ON by LEGACY-SHORELINE-MISAIM-1 (2026-07-31).", passthrough),
L(Presence, "MC2_SHOT_RESULTS", Bool, false, "code", "", ""),
L(Presence, "MC2_SIM_CLOCK_TRACE", Bool, false, "code", "", "", passthrough),
// history: docs/gate-history.md#MC2_SIM_IDENTITY
G("MC2_SIM_IDENTITY", Bool, true, "code", "", "SIM-IDENTITY-HASH-1: emit a world-state hash (sorted-by-partId digest over mover position/rotation/gesture/anim-frame/damage/status/team plus...", passthrough),
// history: docs/gate-history.md#MC2_SIM_IDENTITY_TRACE
G("MC2_SIM_IDENTITY_TRACE", Bool, false, "code", "", "SIM-IDENTITY-HASH-1: emit one `[SIMID v1] simframe=N t=T n=C graded=.. visual=.. strict=..` line per SIM frame to stdout.", passthrough),
// history: docs/gate-history.md#MC2_SIM_IDENTITY_TRACE_DETAIL
G("MC2_SIM_IDENTITY_TRACE_DETAIL", Bool, false, "code", "", "SIM-IDENTITY-HASH-1: with MC2_SIM_IDENTITY_TRACE on, additionally emit one `[SIMID v1 mover]` line per mover per sim frame.", passthrough),
P("MC2_SIM_SPEED", Float, 1.0, "code", "", "", passthrough),
L(Lead0, "MC2_SKIP_STATIC_BUILDINGS", Bool, false, "code", "", ""),
L(Lead0Kill, "MC2_SKIP_STATIC_TREES", Bool, true, "code", "", ""),
L(Presence, "MC2_SKIP_STATIC_TREES_DIAG", Bool, false, "code", "", ""),
// history: docs/gate-history.md#MC2_SKYBOX_FOG_EXCLUDE
L(Lead0Kill, "MC2_SKYBOX_FOG_EXCLUDE", Bool, true, "post", "", "SKYBOX-FOG-EXCLUDE-1/2.", passthrough),
// history: docs/gate-history.md#MC2_SMART_LOAD
L(Lead0Kill, "MC2_SMART_LOAD", Bool, true, "code", "", "SMART-LOAD startup/mission-load perf path (logisticsdata.cpp)."),
L(Presence, "MC2_SMART_LOAD_TRACE", Bool, false, "code", "", ""),
L(NonEmpty, "MC2_SMOKE_ACTIVE", Bool, false, "editor", "", ""),
L(Presence, "MC2_SMOKE_FIXED_TIMESTEP", Bool, false, "gameos", "", "S9D deterministic clock.", passthrough),
G("MC2_SMOKE_LEASE_TTL_SECS", Int, 3600, "harness", "", "smoke harness only (deploy_lease.py): deploy-lease time-to-live in seconds"),
L(Presence, "MC2_SMOKE_MODE", Bool, false, "gameos", "", "", passthrough),
P("MC2_SMOKE_PERF_SAMPLES", Int, 0, "gameos", "", ""),  // default not derivable from the tree
L(Exact1, "MC2_SMOKE_REQUIRE_DIAG_STATE", Bool, false, "harness", "", "smoke harness only (run_smoke.py): a missing diagnostic state dump is a hard fail"),
L(Lead0Kill, "MC2_SMOKE_REQUIRE_FINGERPRINT", Bool, true, "harness", "", "smoke harness only (run_smoke.py): =0 smokes an exe whose build fingerprint does not match HEAD"),
L(Exact1, "MC2_SMOKE_REQUIRE_FRESH", Bool, false, "harness", "", "smoke harness only (run_smoke.py): require a freshly built exe"),
L(Exact1, "MC2_SMOKE_REQUIRE_VISUAL", Bool, false, "harness", "", "smoke harness only (run_smoke.py): a non-PASS visual verdict is a hard fail"),
P("MC2_SMOKE_SEED", Int, 0, "gameos", "", "", passthrough),  // default not derivable from the tree
L(NonEmpty, "MC2_SMOKE_VISIBLE", Bool, false, "harness", "", "smoke harness only (runner.py): show the game window instead of launching it minimized"),
G("MC2_SMOKE_VISUAL_REPORT", String, "", "harness", "", "smoke harness only (run_smoke.py): path of the visual compare report folded into the smoke verdict"),
// history: docs/gate-history.md#MC2_SNAPSHOT_MECH_EXTRACT
L(Lead0, "MC2_SNAPSHOT_MECH_EXTRACT", Bool, false, "mech", "", "MECH-EXTRACTION-0: mech snapshot extraction (observe-only, no GL mutation).", passthrough),
// history: docs/gate-history.md#MC2_SNAPSHOT_STATIC_PROP_BUILD
L(Lead0Kill, "MC2_SNAPSHOT_STATIC_PROP_BUILD", Bool, true, "props", "", "Extraction v3: build v6Packets+meta from RenderSnapshot rows instead of live batcher state (gos_static_prop_batcher.cpp).", passthrough),
// history: docs/gate-history.md#MC2_SNAP_CULL
L(Lead1, "MC2_SNAP_CULL", Bool, false, "props", "", "Extraction v2.3: skip prev-frame zero-instance slots during snapshot-based dispatch (snap-cull opt-in).", passthrough),
L(Presence, "MC2_SOAK_AUTOWIN", Bool, false, "code", "", ""),
L(Presence, "MC2_SOAK_AUTO_PURCHASE", Bool, false, "code", "", "SOAK-AUTO-PURCHASE-1 (MC2_SOAK_AUTO_PURCHASE)."),
L(Presence, "MC2_SOAK_CHECK_SCREENS", Bool, false, "code", "", "SOAK-SCREEN-CHECK-1 (MC2_SOAK_CHECK_SCREENS)."),
L(Presence, "MC2_SOAK_KILL_ENEMY", Bool, false, "code", "", "--- MC2_SOAK_KILL_ENEMY ------------------------------------------------- When set, kills one random live enemy BattleMech before autowin fires so the..."),
L(Presence, "MC2_SOAK_LANCE_RANDOM", Bool, false, "code", "", "SOAK-LANCE-RANDOM-1 (MC2_SOAK_LANCE_RANDOM)."),
L(Presence, "MC2_SOAK_PILOT_PROMOTE", Bool, false, "code", "", "--- MC2_SOAK_PILOT_PROMOTE ---------------------------------------------- Grants a deployed pilot enough skill delta to cross the next rank threshold, then..."),
// history: docs/gate-history.md#MC2_SOAK_SALVAGE_TRADEMOVER
L(Presence, "MC2_SOAK_SALVAGE_TRADEMOVER", Bool, false, "code", "", "ISSUE-42a repro fixture (code/mission.cpp soak harness): with MC2_SOAK_AUTOWIN + MC2_SOAK_KILL_ENEMY, exercises the REAL mid-mission salvage-craft flow...", passthrough),
L(Presence, "MC2_SOAK_STOP_AT_BAY", Bool, false, "code", "", "SOAK-STOP-AT-BAY-1 (MC2_SOAK_STOP_AT_BAY)."),
P("MC2_SOAK_WIN_AFTER_SEC", Float, 5.0, "code", "", ""),
// history: docs/gate-history.md#MC2_SPLIT_PATH_PARITY_TRACE
L(Lead0, "MC2_SPLIT_PATH_PARITY_TRACE", Bool, false, "gameos", "", "META-FIX design C runtime tier (GameOS/gameos/gameos_graphics.cpp gosRenderer::init): =1 emits machine-parseable [SPLIT_PATH_PARITY] marker lines at...", passthrough),
// history: docs/gate-history.md#MC2_SPOTLIGHT_DIRECTION_FIX
L(AtoiNZ, "MC2_SPOTLIGHT_DIRECTION_FIX", Bool, false, "code", "", "SPOTLIGHT-DIRECTION-1: fixes two coordinate-space defects at the building/guard-tower spotlight registration site (mclib/bdactor.cpp).", passthrough),
L(Presence, "MC2_SPOTLIGHT_REAL_TRACE", Bool, false, "unclassified", "", "[SPOTLIGHT_REAL_TRACE v1] (E) Stage 0 / T0.2 baseline."),
L(Presence, "MC2_SPOT_DIAG", Bool, false, "mech", "", "T1.15 SpotLight_ illumination diagnostic - registration probe (bldg class)."),
// history: docs/gate-history.md#MC2_SP_ALPHASCAN_GATE
L(Lead0P, "MC2_SP_ALPHASCAN_GATE", Bool, false, "props", "", "SP-BATCHER-ALPHASCAN-GATE-1: skip the frame-constant per-static-prop alpha re-resolve scan in the batcher when nothing that affects it changed...", passthrough),
// history: docs/gate-history.md#MC2_SP_GLASS_DITHER
L(Lead0Kill, "MC2_SP_GLASS_DITHER", Bool, true, "unclassified", "", "BUILDING-GLASS-1: ordered-dither (Bayer 4x4) translucency for partial-alpha texels on static-prop ALPHA_TEST materials (window glass).", passthrough),
// history: docs/gate-history.md#MC2_SSAO
L(Lead0, "MC2_SSAO", Bool, false, "post", "", "SSAO-GTAO-LITE-MVP-1 (Track V): lightweight half-res screen-space ambient-occlusion grounding pass (gosPostProcess::runSSAO) reusing the scene depth +...", passthrough),
// history: docs/gate-history.md#MC2_SSAO_BLUR
L(Lead0Kill, "MC2_SSAO_BLUR", Bool, true, "post", "", "SSAO-BLUR-1: separable depth-aware (joint bilateral) blur over the half-res AO buffer, inserted between the AO gen pass and the apply pass...", passthrough),
// history: docs/gate-history.md#MC2_SSAO_BLUR_SIGMA
P("MC2_SSAO_BLUR_SIGMA", Float, 0.0, "post", "", "SSAO-BLUR-1 tunable: RELATIVE depth tolerance (|d-dc|/dc) for the SSAO bilateral blur's edge-stopping weight.", passthrough | range(0.001, 1.0)),  // default not derivable from the tree
// history: docs/gate-history.md#MC2_SSAO_DEBUG
L(Lead0, "MC2_SSAO_DEBUG", Bool, false, "post", "", "SSAO-GTAO-LITE-MVP-1: when set with MC2_SSAO=1, the SSAO apply pass OVERWRITES the scene with the half-res AO buffer as grayscale (blend disabled) instead...", passthrough),
// history: docs/gate-history.md#MC2_SSAO_RADIUS
P("MC2_SSAO_RADIUS", Float, 0.0, "post", "", "SSAO-RADIUS-REVERT-1 (VERIFY-DEBT-1): explicit launch-time override for the SSAO world-unit sample radius.", range(0.1, 64.0)),  // default not derivable from the tree
G("MC2_STABLE_LIGHT_SKIP", Bool, false, "props", "", "", passthrough),
G("MC2_STABLE_LIGHT_SKIP_DIAG", Bool, false, "props", "", ""),
G("MC2_STABLE_LIGHT_SKIP_TOUCH", Bool, false, "props", "", ""),
L(Lead1, "MC2_STANDARD_LIT_V1", Bool, false, "mech", "", ""),
// history: docs/gate-history.md#MC2_STATICPROP_BC7
L(Lead0P, "MC2_STATICPROP_BC7", Bool, false, "props", "", "COMPRESSION-BC7-STATICPROP-1: upload BC7-compressed KTX2 static-prop texture arrays through the existing static-prop KTX path (gos_static_prop_batcher.cpp)..."),
// history: docs/gate-history.md#MC2_STATICPROP_MATERIAL_PBR_SLOTS
L(Lead0, "MC2_STATICPROP_MATERIAL_PBR_SLOTS", Bool, false, "props", "", "Static-prop PBR ORM (occlusion-roughness-metallic) texture-array slots + sidecar feed.", passthrough),
// history: docs/gate-history.md#MC2_STATICPROP_ORM_TRACE
L(Presence, "MC2_STATICPROP_ORM_TRACE", Bool, false, "props", "", "Static-prop ORM load trace: logs source path + dims per unique ORM texture."),
L(Lead0Kill, "MC2_STATICPROP_RECIPE_SPOTLIGHT_SKIP", Bool, true, "props", "", "", passthrough),
P("MC2_STATICPROP_TEX_TIER", Int, 512, "props", "", ""),
L(Presence, "MC2_STATIC_C1B_DRAW", Bool, false, "props", "", "C1b: when compute cull is enabled and the indirect command buffer is ready, write the per-type CPU instance counts into the indirect buffer before the draw..."),
L(Presence, "MC2_STATIC_FORCE_ADMIT", Bool, false, "props", "", "2026-05-10 diag: MC2_STATIC_FORCE_ADMIT mirrors registry::flush behavior so the env-var A/B (cull-bypassed vs cull-active) covers BOTH substrate-record...", passthrough),
L(Presence, "MC2_STATIC_INST_DIAG", Bool, false, "props", "", ""),
L(Presence, "MC2_STATIC_LEAF_DIAG", Bool, false, "props", "", ""),
// history: docs/gate-history.md#MC2_STATIC_LIGHT_UPLOAD_SPLIT
L(Lead0Kill, "MC2_STATIC_LIGHT_UPLOAD_SPLIT", Bool, true, "code", "", "STATICPROP-PERMANENT-INSTANCE-LIGHTS-1: upload the immutable static light prefix [0..S) once/dirty-only + the dynamic suffix [S..count) per frame, instead..."),
G("MC2_STATIC_PER_INSTANCE_LIGHT", Bool, true, "props", "", "2026-05-11 per-instance light-idx capture."),
L(Presence, "MC2_STATIC_POP_SPLIT", Bool, false, "props", "", ""),
L(Presence, "MC2_STATIC_POP_SPLIT_CMD_DIAG", Bool, false, "props", "", "Diag (env-gated): dump the static cmd summary so Task 3 can be validated before the draw exists."),
// history: docs/gate-history.md#MC2_STATIC_PROP_AMBIENT_V1
L(Lead0, "MC2_STATIC_PROP_AMBIENT_V1", Bool, false, "props", "", "V-AMBIENT-STATIC-1: hemisphere ambient fill on StaticPropOpaque lane (static_prop.vert).", passthrough),
L(Presence, "MC2_STATIC_PROP_BAKE_SELFTEST", Bool, false, "gameos", "", ""),
// history: docs/gate-history.md#MC2_STATIC_PROP_BUILDING_SHADOW
L(Lead0P, "MC2_STATIC_PROP_BUILDING_SHADOW", Bool, false, "props", "", "SHADOW-STATIC-BUILDINGS-2: replay ALL registered rigid-building recipes (full registry, visibility-independent) into the world-fixed static shadow map, once...", passthrough),
// history: docs/gate-history.md#MC2_STATIC_PROP_BUILDING_SHADOW_REBAKE
L(Lead0Kill, "MC2_STATIC_PROP_BUILDING_SHADOW_REBAKE", Bool, true, "props", "", "STATIC-BLDG-SHADOW-REBAKE-1: re-arm the one-shot world-fixed static-shadow latch (gos_ResetStaticLightMatrix) when a registered building is destroyed --...", passthrough),
// history: docs/gate-history.md#MC2_STATIC_PROP_BUILDING_SHADOW_REBAKE_TEST_DESTROY_AT_FRAME
P("MC2_STATIC_PROP_BUILDING_SHADOW_REBAKE_TEST_DESTROY_AT_FRAME", Int, -1, "code", "", "STATIC-BLDG-SHADOW-REBAKE-1: deterministic repro hook (test-only, no production callers).", passthrough),
// history: docs/gate-history.md#MC2_STATIC_PROP_COLORS_FILL
L(Lead0P, "MC2_STATIC_PROP_COLORS_FILL", Bool, false, "props", "", "STATICPROP-COLORS-FILL-DEBUGONLY-1: per-static-instance Colors SSBO (binding 1) zero-fill.", passthrough),
// history: docs/gate-history.md#MC2_STATIC_PROP_DEBUG_MATERIAL
P("MC2_STATIC_PROP_DEBUG_MATERIAL", Int, 0, "props", "", "V-MATERIAL-DEBUG-1 / V-MATERIAL-PBR-1: per-fragment material debug view on StaticPropOpaque lane (static_prop.frag).", passthrough | range(0, 6)),
L(Presence, "MC2_STATIC_PROP_DEPTH_PREPASS", Bool, false, "props", "", ""),
// history: docs/gate-history.md#MC2_STATIC_PROP_FIXTURE_GLOW
P("MC2_STATIC_PROP_FIXTURE_GLOW", Float, 0.0, "props", "", "FIXTURE-GLOW-1: DEFAULT-OFF on measured evidence.", passthrough | range(0.0, 8.0)),
// history: docs/gate-history.md#MC2_STATIC_PROP_FIXTURE_LIGHTS
P("MC2_STATIC_PROP_FIXTURE_LIGHTS", Float, 1.0, "props", "", "BEACON-FIXTURE-ID-1: DEFAULT-ON.", passthrough | range(0.0, 16.0)),
// history: docs/gate-history.md#MC2_STATIC_PROP_FLUSH_CACHED_BLOB
G("MC2_STATIC_PROP_FLUSH_CACHED_BLOB", Bool, false, "props", "", "STATICPROP-REGISTRY-FLUSH-CACHED-BLOB-2A: replace the per-leaf registry-flush rebuild with cached immutable instance/actor-record blobs bulk-appended into...", passthrough),
// history: docs/gate-history.md#MC2_STATIC_PROP_FLUSH_CACHED_BLOB_COMPARE
G("MC2_STATIC_PROP_FLUSH_CACHED_BLOB_COMPARE", Bool, false, "props", "", "STATICPROP-REGISTRY-FLUSH-CACHED-BLOB-2A diagnostic: with the cached path active, also build the legacy temp instance+record per leaf and compare...", passthrough),
// history: docs/gate-history.md#MC2_STATIC_PROP_FLUSH_COST_SPLIT
L(Exact1, "MC2_STATIC_PROP_FLUSH_COST_SPLIT", Bool, false, "props", "", "SPFLUSH-COST-SPLIT-1: RDTSC cost-split decomposition of GpuStaticPropRegistry::flush().", passthrough),
P("MC2_STATIC_PROP_GLOBAL_CAP", Int, 131072, "props", "", ""),
L(Lead0P, "MC2_STATIC_PROP_GLOBAL_POOL_LEGACY", Bool, false, "props", "", ""),
// history: docs/gate-history.md#MC2_STATIC_PROP_IBL_NIGHT_SCALE
P("MC2_STATIC_PROP_IBL_NIGHT_SCALE", Float, 0.25, "props", "", "IBL-NIGHT-SCALE-1: night-scales the SH-IBL ambient probe on static props.", passthrough | range(0.0, 1.0)),
// history: docs/gate-history.md#MC2_STATIC_PROP_IBL_SH
L(Lead0Kill, "MC2_STATIC_PROP_IBL_SH", Bool, true, "props", "", "V-IBL-STATIC-1: SH-L2 image-based ambient on StaticPropOpaque lane (static_prop.vert).", passthrough),
// history: docs/gate-history.md#MC2_STATIC_PROP_IBL_SH_SET
G("MC2_STATIC_PROP_IBL_SH_SET", String, "", "props", "", "V-IBL-STATIC-2: override the active SH coefficient set by name (e.g.", passthrough),
// history: docs/gate-history.md#MC2_STATIC_PROP_IBL_SH_STRENGTH
P("MC2_STATIC_PROP_IBL_SH_STRENGTH", Float, 0.5, "props", "", "V-IBL-STATIC-1: optional default-strength override for SH-L2 ambient (clamped 0..3).", passthrough | range(0.0, 3.0)),
// history: docs/gate-history.md#MC2_STATIC_PROP_IBL_SUN_ROTATE
L(Lead0, "MC2_STATIC_PROP_IBL_SUN_ROTATE", Bool, false, "props", "", "[DEFAULT-OFF OPT-IN.", passthrough),
// history: docs/gate-history.md#MC2_STATIC_PROP_IBL_SUN_ROTATE_FLOOR
P("MC2_STATIC_PROP_IBL_SUN_ROTATE_FLOOR", Float, 0.45, "props", "", "[VALUE, not a boolean: the floor in the probe's own irradiance units.", passthrough),
G("MC2_STATIC_PROP_LATE_SPAWN_REG", Bool, false, "props", "", "Set MC2_STATIC_PROP_LATE_SPAWN_REG=1 to opt in (default OFF as of 2026-05-20)."),
// history: docs/gate-history.md#MC2_STATIC_PROP_LEGACY_DISPATCH
L(Lead1, "MC2_STATIC_PROP_LEGACY_DISPATCH", Bool, false, "props", "", "v6 DrawPacket+Meta dispatch (default-ON as of v7).", passthrough),
// history: docs/gate-history.md#MC2_STATIC_PROP_LIGHT_FRESHNESS
L(Lead0Kill, "MC2_STATIC_PROP_LIGHT_FRESHNESS", Bool, true, "props", "", "LIGHTROW-FRESHNESS-1: DEFAULT-ON.", passthrough),
L(Lead1, "MC2_STATIC_PROP_LIVE_BUILDER", Bool, false, "props", "", "v8 kill-switch: =1 restores live build + snapshot compare", passthrough),
G("MC2_STATIC_PROP_MISSION_LOAD_REG", Bool, true, "props", "", "Task 9 (Track B): mission-load + late-spawn registration ON by default."),
// history: docs/gate-history.md#MC2_STATIC_PROP_NIGHT_TAGS
P("MC2_STATIC_PROP_NIGHT_TAGS", Int, 0, "props", "", "EDGE-LIGHT-EMISSION-1: restores the stock hot-colour NIGHT vertex tags on the GPU static-prop path.", passthrough | range(0, 2)),
// history: docs/gate-history.md#MC2_STATIC_PROP_NIGHT_TAGS_SCALE
P("MC2_STATIC_PROP_NIGHT_TAGS_SCALE", Float, 0.0, "props", "", "EDGE-LIGHT-EMISSION-1: scales the NIGHT EMISSION DELTA only (night base_light minus day base_light), never the underlying lighting maths, so 0.0 is exactly...", passthrough | range(0.0, 16.0)),  // default not derivable from the tree
// history: docs/gate-history.md#MC2_STATIC_PROP_PBR_V1
L(Lead0, "MC2_STATIC_PROP_PBR_V1", Bool, false, "props", "", "V-MATERIAL-PBR-3: per-fragment Schlick-Fresnel + power-lobe specular on StaticPropOpaque lane (static_prop.frag, inside `#if defined(MC2_USE_VIEW_UNIFORMS)`).", passthrough),
// history: docs/gate-history.md#MC2_STATIC_PROP_PBR_V1_DIAG_SUNFOUND
L(Lead0, "MC2_STATIC_PROP_PBR_V1_DIAG_SUNFOUND", Bool, false, "props", "", "V-MATERIAL-PBR-3-DIAG: diagnostic visualizer for the forwarded sunFound state.", passthrough),
// history: docs/gate-history.md#MC2_STATIC_PROP_PBR_V1_STRENGTH
P("MC2_STATIC_PROP_PBR_V1_STRENGTH", Float, 0.5, "props", "", "V-MATERIAL-PBR-3: optional default-strength override for per-fragment Schlick-Fresnel specular (clamped 0..3).", passthrough | range(0.0, 3.0)),
// history: docs/gate-history.md#MC2_STATIC_PROP_PERSISTENT_BUCKETS
G("MC2_STATIC_PROP_PERSISTENT_BUCKETS", Bool, true, "props", "", "STATICPROP-PERSISTENT-STATIC-BUCKETS-2B-STAGE2 (Mechanism B-reinject): persistent static instance store, rebuilt only on registry-generation change...", passthrough),
// history: docs/gate-history.md#MC2_STATIC_PROP_PERSISTENT_BUCKETS_COMPARE
G("MC2_STATIC_PROP_PERSISTENT_BUCKETS_COMPARE", Bool, false, "props", "", "STATICPROP-PERSISTENT-STATIC-BUCKETS-2B-STAGE2 oracle: FNV-compare the persistent-store-sourced per-type instance stream vs a freshly-rebuilt legacy stream...", passthrough),
L(Lead0, "MC2_STATIC_PROP_PICK", Bool, false, "code", "", ""),
L(Lead0, "MC2_STATIC_PROP_PICK_DEBUG", Bool, false, "code", "", ""),
// history: docs/gate-history.md#MC2_STATIC_PROP_REGISTRY
G("MC2_STATIC_PROP_REGISTRY", Bool, true, "props", "", "GpuStaticPropRegistry enable."),
// history: docs/gate-history.md#MC2_STATIC_PROP_SNAPSHOT_BRIDGE_COMPARE
L(Lead1, "MC2_STATIC_PROP_SNAPSHOT_BRIDGE_COMPARE", Bool, false, "gameos", "", "STATICPROP-SNAPSHOT-BRIDGE-COMPARE-1 gate: default OFF. =1 builds s_spRowCache from registry each frame and compares field-by-field with legacy propBuf.", passthrough),
L(Lead0Kill, "MC2_STATIC_PROP_SNAPSHOT_FILL_DIRTYONLY", Bool, true, "gameos", "", "", passthrough),
L(Lead0, "MC2_STATIC_PROP_TRACE", Bool, false, "props", "", "Stage 2.C+ instrumentation: one-shot registered-types dump."),
// history: docs/gate-history.md#MC2_STATIC_PROXY_RECON
L(Presence, "MC2_STATIC_PROXY_RECON", Bool, false, "props", "", "StaticSceneProxy-arc recon instrumentation (mclib/bdactor.cpp) - counts touchSerialCommit calls for building/tree appearances ahead of the per-frame...", passthrough),
L(Lead0Kill, "MC2_STATIC_RECIPE_RECYCLE", Bool, true, "props", "", ""),
L(Presence, "MC2_STATIC_REG_COVERAGE", Bool, false, "props", "", "STATIC-REGISTRY-COVERAGE-RECON-1: sub-classify the rej_no_static_reg population.", passthrough),
L(Presence, "MC2_STATIC_REG_PREWARM", Bool, false, "code", "", "", passthrough),
L(Presence, "MC2_STATIC_REG_PREWARM_TRACE", Bool, false, "code", "", "", passthrough),
L(Presence, "MC2_STATIC_STALE_DROP_FATAL", Bool, false, "props", "", "Opt-in teeth for CI / bisection: abort on the regression signature.", passthrough),
// history: docs/gate-history.md#MC2_STATIC_UPDATE_SKIP
G("MC2_STATIC_UPDATE_SKIP", Bool, true, "terrain", "", "Skip the per-frame static building/tree update walk when nothing is dirty (GameOS/gameos/gameosmain.cpp, code/terrobj.cpp, code/bldng.cpp).", passthrough),
G("MC2_STATIC_UPDATE_TRACE", Bool, false, "terrain", "", ""),
G("MC2_STRINGS_TRACE", Bool, false, "gameos", "", ""),
L(Presence, "MC2_SUBMIT_TYPEHIST", Bool, false, "props", "", ""),
L(Presence, "MC2_SUBSTRATE_COALESCE_LEGACY", Bool, false, "props", "", ""),
L(Presence, "MC2_SUBSTRATE_COALESCE_TRACE", Bool, false, "props", "", "Step 12.1 - trace gate for noisy non-lifecycle COALESCE diagnostics."),
// history: docs/gate-history.md#MC2_SUN_AXIS_FIX
L(Lead0, "MC2_SUN_AXIS_FIX", Bool, false, "mech", "", "[DEFAULT-OFF, OPT-IN: MC2_SUN_AXIS_FIX=1 enables; unset/0 is the shipped expression bit-for-bit.", passthrough),
// history: docs/gate-history.md#MC2_SUN_ELEVATION_FIX
L(Lead0, "MC2_SUN_ELEVATION_FIX", Bool, false, "code", "", "[DEFAULT-OFF, OPT-IN: MC2_SUN_ELEVATION_FIX=1 enables; unset/0 is the legacy three lines bit-for-bit.", passthrough),
L(Lead1, "MC2_TACMAP_FORMATION_LINE", Bool, false, "code", "", ""),
// history: docs/gate-history.md#MC2_TACTICAL_ARC_OVERLAY
L(Lead0, "MC2_TACTICAL_ARC_OVERLAY", Bool, false, "code", "", "TACTICAL-ARC-OVERLAY-MVP-1: world-space range ring + facing line on each SELECTED mover, drawn in-scene by the debug renderer just before its flush..."),
L(Lead0Kill, "MC2_TACTICAL_OVERVIEW", Bool, true, "code", "", ""),
L(Presence, "MC2_TACTICAL_OVERVIEW_DEBUG", Bool, false, "code", "", "Tactical Overview diagnostic HUD - only under MC2_TACTICAL_OVERVIEW_DEBUG."),
L(Lead0Kill, "MC2_TACTICAL_OVERVIEW_TINT", Bool, true, "code", "", ""),
L(AtoiNZ, "MC2_TACTIC_WEIGHTS", Bool, false, "code", "", "TACTIC-WEIGHTS-A: load [Tactics] block from <missionName>_ai.fit into tacticWeights[]."),
L(AtoiNZ, "MC2_TACTIC_WEIGHTS_B", Bool, false, "code", "", ""),
L(Presence, "MC2_TARGETING_GUARD", Bool, false, "code", "", "", passthrough),
L(Presence, "MC2_TERRAIN_ADMISSION_LEGACY", Bool, false, "terrain", "", ""),
// history: docs/gate-history.md#MC2_TERRAIN_AMBIENT_FLOOR
P("MC2_TERRAIN_AMBIENT_FLOOR", Float, 0.35, "terrain", "", "TERRAIN-AMBIENT-FLOOR-2: terrain-only, albedo-scaled ambient floor in terrain_lod_chunk.frag.", passthrough),
// history: docs/gate-history.md#MC2_TERRAIN_AMBIENT_FLOOR_NIGHT
P("MC2_TERRAIN_AMBIENT_FLOOR_NIGHT", Float, 0.0, "terrain", "", "TERRAIN-AMBIENT-FLOOR-NIGHT-1: 0..1 FLOOR under TERRAIN-AMBIENT-FLOOR-2's daylight gate; the uploaded gate becomes max(dayGate, floor).", passthrough),
// history: docs/gate-history.md#MC2_TERRAIN_AMBIENT_FLOOR_TRACE
L(Presence, "MC2_TERRAIN_AMBIENT_FLOOR_TRACE", Bool, false, "terrain", "", "TERRAIN-AMBIENT-FLOOR-2 trace: prints ONE line at the first terrain-chunk uniform upload giving the resolved floor strength, the daylight gate ACTUALLY...", passthrough),
L(Lead0, "MC2_TERRAIN_CEMENT_DIAG_CONNECT", Bool, false, "terrain", "", ""),
// history: docs/gate-history.md#MC2_TERRAIN_CLIFF_DEBUG
P("MC2_TERRAIN_CLIFF_DEBUG", Int, 0, "terrain", "", "TERRAIN-CLIFF-DEBUG: bounded debug-viz for the LOD-chunk terrain cliff material path (u_cliffDebug in terrain_lod_chunk.frag).", passthrough),
// history: docs/gate-history.md#MC2_TERRAIN_CLIFF_DISPLACE
L(Lead0, "MC2_TERRAIN_CLIFF_DISPLACE", Bool, false, "terrain", "", "SLICE 3: near-field cliff tessellation on the LOD-chunk terrain path.", passthrough),
// history: docs/gate-history.md#MC2_TERRAIN_CLIFF_DISPLACE_MAXTESS
P("MC2_TERRAIN_CLIFF_DISPLACE_MAXTESS", Float, 4.0, "terrain", "", "SLICE 3 knob: max tessellation factor (float; C++ default 4.0, clamped to GL_MAX_TESS_GEN_LEVEL at program build).", passthrough),
// history: docs/gate-history.md#MC2_TERRAIN_CLIFF_HEIGHT_NORMAL_STRENGTH
P("MC2_TERRAIN_CLIFF_HEIGHT_NORMAL_STRENGTH", Float, 2.0, "terrain", "", "TERRAIN-CLIFF-HEIGHT-NORMAL-1 knob (float, default 2.0): derives the cliff SHADING normal from the HEIGHT GRADIENT of the cooked cliff displacement...", passthrough),
// history: docs/gate-history.md#MC2_TERRAIN_CLIFF_POM
L(Lead0, "MC2_TERRAIN_CLIFF_POM", Bool, false, "terrain", "", "TERRAIN-CLIFF-POM-1: triplanar Parallax Occlusion Mapping on cliff faces.", passthrough),
// history: docs/gate-history.md#MC2_TERRAIN_CLIFF_POM_DEPTH
P("MC2_TERRAIN_CLIFF_POM_DEPTH", Float, 12.0, "terrain", "", "TERRAIN-CLIFF-POM-1 knob: parallax depth = world-unit rock height scale relative to the CLIFF tiling (ts=256 wu/repeat).", passthrough),
// history: docs/gate-history.md#MC2_TERRAIN_CLIFF_POM_STEPS
P("MC2_TERRAIN_CLIFF_POM_STEPS", Float, 24.0, "terrain", "", "TERRAIN-CLIFF-POM-1 knob: max POM march layers per plane, clamped 8..32 (default 24; compile-constant loop cap 32).", passthrough),
L(Lead0, "MC2_TERRAIN_CLIFF_SHADOW_FLOOR", Bool, false, "terrain", "", ""),
// history: docs/gate-history.md#MC2_TERRAIN_CLIFF_TRIPLANAR
L(Lead0, "MC2_TERRAIN_CLIFF_TRIPLANAR", Bool, false, "terrain", "", "TERRAIN-CLIFF-MATERIAL-TRIPLANAR-1: on steep terrain cells (macro slope, cliffBlend=smoothstep(0.85,0.55,|macroNz|)), project the MAT_LAYER_MARBLE_CLIFF...", passthrough),
// history: docs/gate-history.md#MC2_TERRAIN_CLIFF_TRIPLANAR_STRENGTH
P("MC2_TERRAIN_CLIFF_TRIPLANAR_STRENGTH", Float, 1.0, "terrain", "", "TERRAIN-CLIFF-MATERIAL-TRIPLANAR-1 knob: triplanar rock-normal tilt strength (float, default 1.0).", passthrough),
// history: docs/gate-history.md#MC2_TERRAIN_CONTROLMAP
L(Lead0, "MC2_TERRAIN_CONTROLMAP", Bool, false, "terrain", "", "TERRAIN-CONTROLMAP-SAMPLE-1: load an authored per-mission RGBA control-map sidecar (data/missions/<stem>.beauty/control_map.png, vertex-resolution) that...", passthrough),
L(Lead0, "MC2_TERRAIN_CONTROLMAP_ALBEDO", Bool, false, "terrain", "", ""),
P("MC2_TERRAIN_CONTROLMAP_ALBEDO_STRENGTH", Float, 0.0, "terrain", "", "", passthrough),  // default not derivable from the tree
// history: docs/gate-history.md#MC2_TERRAIN_CONTROLMAP_FILE
L(Presence, "MC2_TERRAIN_CONTROLMAP_FILE", Bool, false, "terrain", "", "TERRAIN-CONTROLMAP-SAMPLE-1: override the control-map sidecar path (default data/missions/<stem>.beauty/control_map.png).", passthrough),
L(Exact1, "MC2_TERRAIN_COST_SPLIT", Bool, false, "terrain", "", "", passthrough),
L(Presence, "MC2_TERRAIN_CULL_PROBE", Bool, false, "code", "", ""),
L(Presence, "MC2_TERRAIN_CULL_WIDE", Bool, false, "terrain", "", "S2.13: MC2_TERRAIN_CULL_WIDE=1 forces the angular sphere cull to admit-all (constants set to 1e9)."),
// history: docs/gate-history.md#MC2_TERRAIN_DEBUG_MODE
P("MC2_TERRAIN_DEBUG_MODE", Float, 0.0, "gameos", "", "TERRAIN-DEBUG-VIEWS-1: terrain fragment-shader debug-mode selector for the tessellated terrain path (gos_terrain.frag tessDebug.x)."),  // default not derivable from the tree
// history: docs/gate-history.md#MC2_TERRAIN_DECAL
L(Lead0P, "MC2_TERRAIN_DECAL", Bool, false, "props", "", "TERRAIN-DECAL-SLICE-0A: terrain mesh-decal system.", passthrough),
// history: docs/gate-history.md#MC2_TERRAIN_DECAL_COLORBLEND
P("MC2_TERRAIN_DECAL_COLORBLEND", Float, 0.35, "props", "", "TERRAIN-DECAL-COLORBLEND-1: RVT-style terrain-colormap blend (float 0..1, default 0.35) for the CLIFF_WALL mesh-decal.", passthrough | range(0.0, 1.0)),
// history: docs/gate-history.md#MC2_TERRAIN_DECAL_FILL
P("MC2_TERRAIN_DECAL_FILL", Float, 0.2, "props", "", "TERRAIN-DECAL-FILL-1: ambient/fill floor (float 0..1, default 0.20) for the CLIFF_WALL mesh-decal's shadow side.", passthrough | range(0.0, 1.0)),
P("MC2_TERRAIN_DECAL_LATERAL", Float, 0.0, "code", "", "TERRAIN-DECAL-SLICE-0B: lateral nudge (float world units, default 0.0) along the wall 'tangent' (contour) axis - side-to-side placement.", passthrough),
P("MC2_TERRAIN_DECAL_LIFT", Float, 0.0, "code", "", "TERRAIN-DECAL-SLICE-0B: vertical nudge (float world units, default 0.0) along world-up - lifts/lowers the CLIFF_WALL mesh-decal.", passthrough),
P("MC2_TERRAIN_DECAL_OFFSET", Float, 8.0, "code", "", "TERRAIN-DECAL-SLICE-0B: outward offset distance (float world units, default 8.0) along the wall 'facing' axis - replaces the hardcoded 8.0 Slice-0A z-fight...", passthrough),
// history: docs/gate-history.md#MC2_TERRAIN_DECAL_PITCH
P("MC2_TERRAIN_DECAL_PITCH", Float, 0.0, "code", "", "TERRAIN-DECAL-PITCH: pitch/lean (float degrees, default 0.0) of the wall about the horizontal tangent (contour) axis - positive leans the wall TOP backward...", passthrough),
// history: docs/gate-history.md#MC2_TERRAIN_DECAL_SCALE
P("MC2_TERRAIN_DECAL_SCALE", Float, 1.0, "code", "", "TERRAIN-DECAL-SLICE-0B: uniform scale (float, default 1.0) of the CLIFF_WALL mesh-decal - multiplies the face-frame's 3 basis rows before translation.", passthrough),
L(Presence, "MC2_TERRAIN_DECAL_TRACE", Bool, false, "props", "", "", passthrough),
// history: docs/gate-history.md#MC2_TERRAIN_DECAL_YAW
P("MC2_TERRAIN_DECAL_YAW", Float, 0.0, "code", "", "TERRAIN-DECAL-SLICE-0B: yaw rotation (float degrees, default 0.0) of the wall facing/tangent about world-up - corrects the outward-facing direction.", passthrough),
L(Lead0, "MC2_TERRAIN_DETAIL_ANTITILE", Bool, false, "terrain", "", ""),
P("MC2_TERRAIN_DETAIL_FADE_END", Float, 3000.0, "gameos", "", ""),
P("MC2_TERRAIN_DETAIL_FADE_START", Float, 1000.0, "gameos", "", ""),
P("MC2_TERRAIN_DETAIL_LOD1_STRENGTH", Float, 0.4, "terrain", "", ""),
P("MC2_TERRAIN_DETAIL_MACRO_STRENGTH", Float, 0.0, "terrain", "", "", divergent),  // default not derivable from the tree
L(Lead0Kill, "MC2_TERRAIN_EDGE_FEATHER", Bool, true, "terrain", "", ""),
P("MC2_TERRAIN_EDGE_FEATHER_STRENGTH", Float, 1.0, "terrain", "", ""),
// history: docs/gate-history.md#MC2_TERRAIN_HEIGHT_RESAMPLE_FACTOR
P("MC2_TERRAIN_HEIGHT_RESAMPLE_FACTOR", Int, 1, "terrain", "", "TERRAIN-RESAMPLE-1: CPU bilinear resample factor for the per-mission terrain height texture used by TERRAIN-NORMALS-FROM-HEIGHT-1."),
L(Lead0Kill, "MC2_TERRAIN_INDIRECT", Bool, true, "terrain", "", "", passthrough),
L(Presence, "MC2_TERRAIN_INDIRECT_CPU_FALLBACK", Bool, false, "terrain", "", "VPL retirement step 1 (2026-05-14): CPU thin-record pack demoted behind MC2_TERRAIN_INDIRECT_CPU_FALLBACK."),
L(Lead0Kill, "MC2_TERRAIN_INDIRECT_MINE", Bool, true, "terrain", "", "", passthrough),
L(Lead0Kill, "MC2_TERRAIN_INDIRECT_OVERLAY", Bool, true, "terrain", "", "", passthrough),
L(Exact1, "MC2_TERRAIN_INDIRECT_OVERLAY_PARITY_CHECK", Bool, false, "terrain", "", "", passthrough),
L(Exact1, "MC2_TERRAIN_INDIRECT_PARITY_CHECK", Bool, false, "terrain", "", "", passthrough),
L(Presence, "MC2_TERRAIN_INDIRECT_THINEMIT_TRACE", Bool, false, "terrain", "", "Step 1b-1 (cpu-pack-retirement plan ?2 A1): the M2 thin-record emit scope is orphaned by Fix B (commit 005ebc7) - the GPU compute shader..."),
L(Exact1, "MC2_TERRAIN_INDIRECT_TRACE", Bool, false, "terrain", "", "", passthrough),
L(Lead0Kill, "MC2_TERRAIN_LIGHTING_GPU", Bool, true, "terrain", "", "", passthrough),
L(Presence, "MC2_TERRAIN_LIGHTING_PARITY", Bool, false, "terrain", "", "", passthrough),
// history: docs/gate-history.md#MC2_TERRAIN_LIGHT_POOLS
L(Lead0Kill, "MC2_TERRAIN_LIGHT_POOLS", Bool, true, "terrain", "", "NIGHT-LIGHT-POOLS-1: per-fragment terrain light-pool loop on the LIVE LOD-chunk terrain path (terrain_lod_chunk.frag).", passthrough),
L(Lead0Kill, "MC2_TERRAIN_LOD_CHECKER_DIAG", Bool, true, "terrain", "", ""),
// history: docs/gate-history.md#MC2_TERRAIN_LOD_CHUNK
L(Lead0Kill, "MC2_TERRAIN_LOD_CHUNK", Bool, true, "terrain", "", "LOD-chunk terrain renderer (mclib/terrain.cpp/terrain.h), the primary tessellation-free terrain path."),
P("MC2_TERRAIN_LOD_CHUNK_CEMENT_MAXLOD", Int, 0, "terrain", "", ""),
P("MC2_TERRAIN_LOD_CHUNK_DIAG", Int, 0, "terrain", "", "", passthrough),
P("MC2_TERRAIN_LOD_CHUNK_DIST_SCALE", Float, 1.0, "terrain", "", ""),
L(Presence, "MC2_TERRAIN_LOD_CHUNK_FORCE_COLOR", Bool, false, "terrain", "", "Resolved-once cached mirrors of the per-frame terrain-uniform env gates."),
P("MC2_TERRAIN_LOD_CHUNK_FORCE_LOD", Int, -1, "terrain", "", ""),
L(Presence, "MC2_TERRAIN_LOD_CHUNK_NO_APRON", Bool, false, "terrain", "", ""),
L(Presence, "MC2_TERRAIN_LOD_CHUNK_NO_CULL", Bool, false, "terrain", "", "Diagnostic: one-shot dump of planes + first superchunk AABB + camera pos."),
L(Presence, "MC2_TERRAIN_LOD_CHUNK_NO_SKIRTS", Bool, false, "terrain", "", ""),
L(Presence, "MC2_TERRAIN_LOD_CHUNK_NO_STITCH", Bool, false, "terrain", "", "Phase 10.2b: PER-EDGE mask."),
P("MC2_TERRAIN_LOD_CHUNK_SKIRT_MAX", Float, 256.0, "terrain", "", ""),
L(Presence, "MC2_TERRAIN_LOD_DEPTH_ALWAYS", Bool, false, "terrain", "", "Phase 10.3: chunk terrain is OPAQUE and must EXPLICITLY own depth/blend/ cull state."),
L(Lead0Kill, "MC2_TERRAIN_LOD_GEOMORPH", Bool, true, "terrain", "", ""),
P("MC2_TERRAIN_LOD_MORPH_START", Float, 0.6, "terrain", "", "", range(0.0, 0.95)),
G("MC2_TERRAIN_MACRO_VARIATION", Bool, false, "terrain", "", ""),
P("MC2_TERRAIN_MACRO_VARIATION_STRENGTH", Float, 1.0, "terrain", "", ""),
L(Lead0, "MC2_TERRAIN_MASK_DISPATCH", Bool, false, "terrain", "", "Cached at Init() time; re-read here for the IsMaskDispatchEnabled() path which may be called before Init() (safe: returns false if not init'd).", passthrough),
L(Lead0Kill, "MC2_TERRAIN_MASK_DISPATCH_SOLID", Bool, true, "terrain", "", ""),
L(Lead0Kill, "MC2_TERRAIN_MASK_DISPATCH_WATER", Bool, true, "terrain", "", ""),
L(Lead0, "MC2_TERRAIN_MASK_UPLOAD_STATS", Bool, false, "gameos", "", ""),
L(Lead0, "MC2_TERRAIN_MATERIAL_LIB", Bool, false, "terrain", "", "", passthrough),
G("MC2_TERRAIN_MATERIAL_LIB_FILE", Bool, false, "terrain", "", "", passthrough),  // default not derivable from the tree
// history: docs/gate-history.md#MC2_TERRAIN_MATERIAL_TEXTURES
L(Lead0, "MC2_TERRAIN_MATERIAL_TEXTURES", Bool, false, "terrain", "", "TERRAIN-MATERIAL-TEXTURES-1: 6-layer BC7 sRGB GL_TEXTURE_2D_ARRAY albedo (rock/grass/dirt/concrete/snow/cliff from...", passthrough),
// history: docs/gate-history.md#MC2_TERRAIN_MATERIAL_TEXTURES_STRENGTH
P("MC2_TERRAIN_MATERIAL_TEXTURES_STRENGTH", Float, -1.0, "terrain", "", "TERRAIN-MATERIAL-TEXTURES-1 knob: albedo mix strength override (float, clamped 0..1).", passthrough | range(0.0, 1.0)),
L(Presence, "MC2_TERRAIN_MINE_AB", Bool, false, "terrain", "", ""),
L(Lead0Kill, "MC2_TERRAIN_NORMAL_ARRAY", Bool, true, "gameos", "", ""),
// history: docs/gate-history.md#MC2_TERRAIN_OVERLAY_V2
L(Lead0, "MC2_TERRAIN_OVERLAY_V2", Bool, false, "terrain", "", "TERRAIN-OVERLAY-V2-PARITY-1: composite an authored world-XY-bounded cement/pad/runway overlay sidecar (data/missions/<stem>.beauty/overlay_v2.png...", passthrough),
// history: docs/gate-history.md#MC2_TERRAIN_OVERLAY_V2_DECAL_SUPPRESS
L(Lead0Kill, "MC2_TERRAIN_OVERLAY_V2_DECAL_SUPPRESS", Bool, true, "terrain", "", "TERRAIN-OVERLAY-V2-DECAL-SUPPRESS-1: in the static decal bake, skip legacy runway/road/cement decals for tiles fully inside the armed OVERLAY_V2 sidecar..."),
// history: docs/gate-history.md#MC2_TERRAIN_OVERLAY_V2_FILE
L(Presence, "MC2_TERRAIN_OVERLAY_V2_FILE", Bool, false, "terrain", "", "TERRAIN-OVERLAY-V2-PARITY-1: override the overlay_v2 sidecar path (default data/missions/<stem>.beauty/overlay_v2.png).", passthrough),
L(Presence, "MC2_TERRAIN_PICK_PARITY", Bool, false, "code", "", "PARITY DIAGNOSTIC: when MC2_TERRAIN_PICK_PARITY is set we compute BOTH the heightfield raycast hit AND the legacy quadList result for the same screen..."),
// history: docs/gate-history.md#MC2_TERRAIN_POM
L(Lead0, "MC2_TERRAIN_POM", Bool, false, "terrain", "", "TERRAIN-CHUNK-POM-1: real per-fragment tangent-space view-vector POM on the LOD-chunk terrain (rock/grass detail layers).", passthrough),
// history: docs/gate-history.md#MC2_TERRAIN_POM_FAR
P("MC2_TERRAIN_POM_FAR", Float, 3500.0, "terrain", "", "TERRAIN-CHUNK-POM-1 knob: POM distance-fade band end in world units (default 3500).", passthrough),
// history: docs/gate-history.md#MC2_TERRAIN_POM_NEAR
P("MC2_TERRAIN_POM_NEAR", Float, 1500.0, "terrain", "", "TERRAIN-CHUNK-POM-1 knob: POM distance-fade band start in world units (default 1500; 1 tile = 384 wu).", passthrough),
// history: docs/gate-history.md#MC2_TERRAIN_POM_SCALE
P("MC2_TERRAIN_POM_SCALE", Float, -1.0, "terrain", "", "TERRAIN-CHUNK-POM-1 knob: POM march scale override (float > 0).", passthrough),
// history: docs/gate-history.md#MC2_TERRAIN_POM_STEPS
P("MC2_TERRAIN_POM_STEPS", Float, 16.0, "terrain", "", "TERRAIN-CHUNK-POM-1 knob: max POM march layers, accepted 4..16 (else default 16; hard compile-constant cap 16).", passthrough),
L(Presence, "MC2_TERRAIN_PROBE", Bool, false, "editor", "", ""),
L(Presence, "MC2_TERRAIN_RAYCAST_PICK", Bool, false, "code", "", "Phase 7B: heightfield raycast picker - replaces the quadList AABB/ screen-triangle scan when MC2_TERRAIN_LOD_CHUNK=1."),
L(Exact1, "MC2_TERRAIN_RUNTIME_DECALS", Bool, false, "terrain", "", ""),
L(Exact1, "MC2_TERRAIN_RUNTIME_GROUNDING", Bool, false, "terrain", "", ""),
L(Lead1, "MC2_TERRAIN_RUNTIME_PARITY", Bool, false, "terrain", "", ""),
// history: docs/gate-history.md#MC2_TERRAIN_SHADOW_GUARD_CLAMPED_SUN
L(Lead0Kill, "MC2_TERRAIN_SHADOW_GUARD_CLAMPED_SUN", Bool, true, "gameos", "", "TERRAIN-SHADOW-GUARD-1: the TERRAIN-side twin of SHADOW-SUNCLAMP-1, and the one that actually moves world pixels.", passthrough),
L(Lead0Kill, "MC2_TERRAIN_SHORELINE", Bool, true, "terrain", "", "", passthrough),
P("MC2_TERRAIN_SHORELINE_EDGE_JITTER", Float, 4.0, "terrain", "", "", range(0.0, 32.0)),
L(Presence, "MC2_TERRAIN_SHORELINE_FILE", Bool, false, "terrain", "", "", passthrough),
P("MC2_TERRAIN_SHORELINE_FOAM", Float, 1.0, "terrain", "", "", range(0.0, 2.0)),
P("MC2_TERRAIN_SHORELINE_FOAM_HEIGHT", Float, 5.0, "terrain", "", ""),
P("MC2_TERRAIN_SHORELINE_FOAM_RUN", Float, 5.0, "terrain", "", ""),
P("MC2_TERRAIN_SHORELINE_STRENGTH", Float, 1.0, "terrain", "", "", range(0.0, 2.0)),
P("MC2_TERRAIN_SHORELINE_WET_HEIGHT", Float, 16.0, "terrain", "", ""),
P("MC2_TERRAIN_SHORELINE_WET_RUN", Float, 16.0, "terrain", "", ""),
L(Lead0, "MC2_TERRAIN_SLOPE_BIAS", Bool, false, "terrain", "", ""),
P("MC2_TERRAIN_SLOPE_BIAS_STRENGTH", Float, 1.0, "terrain", "", ""),
P("MC2_TERRAIN_SNOW_BRIGHTNESS_DAMPEN", Float, 0.78, "terrain", "", "Snow brightness dampen: <1 darkens detected-snow fragments."),
L(Lead0Kill, "MC2_TERRAIN_SOLID_NARROW", Bool, true, "terrain", "", ""),
L(Exact1, "MC2_TERRAIN_SOLID_WINDOW_PARITY", Bool, false, "terrain", "", ""),
L(Lead0, "MC2_TERRAIN_SURFACE", Bool, false, "terrain", "", "", passthrough),
L(Presence, "MC2_TERRAIN_SURFACE_TRACE", Bool, false, "terrain", "", "", passthrough),
// history: docs/gate-history.md#MC2_TERRAIN_TESS
L(Lead0Kill, "MC2_TERRAIN_TESS", Bool, true, "gameos", "", "TCS/TES terrain tessellation (gameos_graphics.cpp).", passthrough),
L(Lead0, "MC2_TERRAIN_TILE_CENSUS", Bool, false, "terrain", "", ""),
L(Lead0, "MC2_TERRAIN_VISUAL_DISPLACE", Bool, false, "terrain", "", "", passthrough),
P("MC2_TERRAIN_VISUAL_DISPLACE_FAR", Float, 1.0, "terrain", "", "", passthrough | range(0.0, 1.0)),
// history: docs/gate-history.md#MC2_TERRAIN_VISUAL_DISPLACE_OBJFADE
L(Lead0Kill, "MC2_TERRAIN_VISUAL_DISPLACE_OBJFADE", Bool, true, "terrain", "", "TERRAIN-VISUAL-HEIGHT-SAMPLE-1: when visual terrain displacement is on, fade object grounding across the displaced-vs-gameplay height delta so props/units..."),
P("MC2_TERRAIN_VISUAL_DISPLACE_OBJFADE_RADIUS", Float, 256.0, "code", "", "", range(32.0, 1024.0)),
// history: docs/gate-history.md#MC2_TERRAIN_VISUAL_HEIGHT
L(Lead0, "MC2_TERRAIN_VISUAL_HEIGHT", Bool, false, "terrain", "", "TERRAIN-VISUAL-HEIGHT: loads the 4x-res VISUAL heightfield bake into an SSBO for corner-pinned interior displacement (mclib/terrain.cpp).", passthrough),
L(Presence, "MC2_TERRAIN_VISUAL_HEIGHT_FILE", Bool, false, "terrain", "", "", passthrough),
// history: docs/gate-history.md#MC2_TEST_SPAWN
L(Lead1, "MC2_TEST_SPAWN", Bool, false, "code", "", "GAMEPLAY-DRIVER-1: =1 arms the test-only spawn/teleport verbs on the MC2_UI_COMMAND_FILE channel.", passthrough),
// history: docs/gate-history.md#MC2_TEXMGR_COMPRESSED_UPLOAD
L(Lead0Kill, "MC2_TEXMGR_COMPRESSED_UPLOAD", Bool, true, "code", "", "TEXMGR-COMPRESSED-UPLOAD-1: teach MC_TextureManager to load a BC7 .ktx2 sidecar (same path/stem as the source data/textures texture, extension -> .ktx2) and..."),
P("MC2_TEXMGR_KTX_PRIMARY", Int, 1, "mech", "", "", passthrough),
L(Presence, "MC2_TEXMGR_LOAD_TRACE", Bool, false, "code", "", "", passthrough),
L(Presence, "MC2_TEX_LIFECYCLE_TRACE", Bool, false, "props", "", "MC2_TEX_LIFECYCLE_TRACE=1 - same env flag used by mclib/txmmgr.cpp, mclib/msl.cpp, GameOS/gameos/gos_static_prop_batcher.cpp so all [TEX_LIFECYCLE v1]..."),
L(Presence, "MC2_TEX_LIFECYCLE_TRACE_VERBOSE", Bool, false, "code", "", ""),
L(Presence, "MC2_TGL_POOL_TRACE", Bool, false, "gameos", "", ""),
// history: docs/gate-history.md#MC2_THIN_CANARY
L(Lead1, "MC2_THIN_CANARY", Bool, false, "terrain", "", "TRACKV: gates Probe-6 thin-record canary readback (per-frame ~80KB glGetBufferSubData x2 + CPU recipeIdx/flags compare, diagnostic for the corrupt-recipeIdx..."),
L(Presence, "MC2_THIN_DEBUG", Bool, false, "terrain", "", "", passthrough),
L(Presence, "MC2_TOBJ_COST_SPLIT", Bool, false, "unclassified", "", "[TOBJSPLIT v1] env-gated RDTSC cost split for the recalcBounds slice.", passthrough),
L(Presence, "MC2_TOBJ_PARITY", Bool, false, "terrain", "", "[TOBJPARITY v1] env-gated STANDALONE readback-vs-coarse superset-parity instrument.", passthrough),
// history: docs/gate-history.md#MC2_TONEMAP_V2
L(Lead1, "MC2_TONEMAP_V2", Bool, false, "post", "", "POST-FX-TONEMAP-V2: optional filmic tonemap curve on the final composited colour (postprocess.frag).", passthrough),
L(Presence, "MC2_TRACE_EXIT", Bool, false, "gameos", "", ""),
// history: docs/gate-history.md#MC2_TREE_DIAG_TRACE
L(Presence, "MC2_TREE_DIAG_TRACE", Bool, false, "props", "", "[DEFAULT-OFF, but the OFF token is UNSET, not '0'.]", passthrough),
L(Presence, "MC2_TREE_REG_TRACE", Bool, false, "props", "", ""),
L(Presence, "MC2_TUBE_PROFILE_LOG", Bool, false, "code", "", ""),
// history: docs/gate-history.md#MC2_TUNE_VFX_ADDITIVE_BRIGHTNESS
P("MC2_TUNE_VFX_ADDITIVE_BRIGHTNESS", Float, 1.0, "gameos", "", "VFX-TUNING-UI-1: startup-default for the additive-only GPU-particle RGB brightness scale (particle_billboard.frag u_vfxAdditiveBrightness, applied ONLY to...", passthrough),
// history: docs/gate-history.md#MC2_TUNE_VFX_AGE_FADE
P("MC2_TUNE_VFX_AGE_FADE", Float, 0.0, "gameos", "", "VFX-SHADER-AGE-FADE-PARITY-1: soft death fade for oracle-harvested particles (p.lifetime==1.0f sentinel) in the final 30% of normalized age.", passthrough),
// history: docs/gate-history.md#MC2_TUNE_VFX_ALPHA_SCALE
P("MC2_TUNE_VFX_ALPHA_SCALE", Float, 1.0, "gameos", "", "VFX-TUNING-UI-1: startup-default for the GPU-particle alpha (opacity) scale (particle_billboard.frag u_vfxAlphaScale, applied to ALL particles).", passthrough),
// history: docs/gate-history.md#MC2_TUNE_VFX_BRIGHTNESS
P("MC2_TUNE_VFX_BRIGHTNESS", Float, 1.0, "gameos", "", "VFX-TUNING-UI-1: startup-default for the global GPU-particle RGB brightness scale (particle_billboard.frag u_vfxBrightness, applied to ALL particles).", passthrough),
// history: docs/gate-history.md#MC2_TUNE_VFX_LIT_STRENGTH
P("MC2_TUNE_VFX_LIT_STRENGTH", Float, 0.7, "gameos", "", "VFX-LIT-PARTICLES-MVP-1: startup-default for the scene-lit smoke/dust strength (particle_billboard.frag u_vfxLitStrength), applied ONLY to alpha draw groups...", passthrough),
L(Lead1, "MC2_TXMMGR_BOUNDS_TRACE", Bool, false, "code", "", "[TXMMGR_BOUNDS v1] - TXMMGR-BOUNDS-HARDEN-1 exhaustion counters."),
L(Lead1, "MC2_TXM_LEAK_TRACE", Bool, false, "code", "", "[TXMMGR_TEXTURE_AUDIT v1] - texture-node-slot leak instrumentation."),
L(Presence, "MC2_TYPE_TABLE_CAND_LOG", Bool, false, "props", "", "", passthrough),
L(Presence, "MC2_TYPE_TABLE_CAND_VERBOSE", Bool, false, "props", "", "", passthrough),
L(Lead0Kill, "MC2_UI_ASPECT_ANCHOR", Bool, true, "gameos", "", ""),
// history: docs/gate-history.md#MC2_UI_COMMAND_FILE
L(NonEmpty, "MC2_UI_COMMAND_FILE", Bool, false, "code", "", "UI-COMMAND-CHANNEL-1 + GAMEPLAY-DRIVER-1/2: =<path> polls that file once per frame (MissionInterfaceManager::update) and executes its lines as commands.", passthrough),
G("MC2_UI_DEFS_IGNORE_PACKAGE_VERSION", Bool, true, "editor", "", ""),
L(Presence, "MC2_UI_DEFS_LOADSCREEN", Bool, false, "editor", "", ""),
L(Presence, "MC2_UI_DEFS_RESULTS", Bool, false, "editor", "", ""),
L(Lead0, "MC2_UI_DEFS_TRACE", Bool, false, "unclassified", "", ""),
// history: docs/gate-history.md#MC2_UI_IMGUI_INFO
L(Lead0, "MC2_UI_IMGUI_INFO", Bool, false, "code", "", "INFO-IMGUI-SLICE-1: =1 renders the in-mission unit-Info panel as an ImGui window (unit name + health bar + loadout list with scrollbar) laid out inside..."),
// history: docs/gate-history.md#MC2_UI_IMGUI_OBJ
L(Lead0, "MC2_UI_IMGUI_OBJ", Bool, false, "code", "", "OBJ-IMGUI-SLICE-1: =1 renders the in-mission OBJECTIVES overlay as ImGui (baked left-edge frame from tools/ui_art_reconstruct.py + procedural..."),
// history: docs/gate-history.md#MC2_UI_IMGUI_PAUSE
L(Lead0, "MC2_UI_IMGUI_PAUSE", Bool, false, "code", "", "PAUSE-IMGUI-SLICE-1: =1 renders the in-mission pause menu as an ImGui window (GAME PAUSED + objectives/save/load/options/abort/exit/return) laid out inside..."),
// history: docs/gate-history.md#MC2_UI_IMGUI_TIMER
L(Lead0, "MC2_UI_IMGUI_TIMER", Bool, false, "code", "", "SMALLPANELS-IMGUI-SLICE-1: =1 renders the in-mission mission-timer display as ImGui (right-aligned MM:SS via HudFont + the TimerStatic atlas tile) laid out..."),
// history: docs/gate-history.md#MC2_UI_IMGUI_VIDEO
L(Lead0, "MC2_UI_IMGUI_VIDEO", Bool, false, "code", "", "SMALLPANELS-IMGUI-SLICE-1: =1 renders the in-mission comm-video panel as ImGui - VideoStatic chrome tiles + the decoder's frame texture at the same..."),
L(Presence, "MC2_UI_PASS_MEASURE", Bool, false, "gameos", "", "UI-PASS-DRAWCOUNT-AMBIENT-MEASURE-1: observe-only telemetry inside the existing PassIdentity::UI RAII scope.", passthrough),
G("MC2_UNIT_DEFINITION_SOURCE", Bool, false, "code", "", "", passthrough),
// history: docs/gate-history.md#MC2_UNIT_PROFILE_DATA
L(Lead0, "MC2_UNIT_PROFILE_DATA", Bool, false, "unclassified", "", "UNIT-PROFILE-ARC: data-driven unit+equipment definition source (code/unitprofile.cpp).", passthrough),
L(Presence, "MC2_VANISH_PROBE", Bool, false, "code", "", ""),
L(NonEmpty, "MC2_VEGETATION_ATLAS", Bool, false, "gameos", "", "Allow env-var override at runtime too.", passthrough),
L(Lead0, "MC2_VEGETATION_CARDS", Bool, false, "gameos", "", "", passthrough),
P("MC2_VEGETATION_DENSITY", Int, 25, "code", "", ""),
P("MC2_VEGETATION_MAX", Int, 5000000, "code", "", ""),
L(Lead0, "MC2_VEG_DEBUG_FORCE_VISIBLE", Bool, false, "gameos", "", ""),
P("MC2_VEG_MAX_DIST", Float, 4096.0, "gameos", "", ""),
// history: docs/gate-history.md#MC2_VEHICLE_AMBIENT_FLOOR
L(Lead0Kill, "MC2_VEHICLE_AMBIENT_FLOOR", Bool, true, "code", "", "VEHICLE-AMBIENT-FLOOR-1: DEFAULT-ON.", passthrough),
// history: docs/gate-history.md#MC2_VEHICLE_LIT_PATH
L(Lead0Kill, "MC2_VEHICLE_LIT_PATH", Bool, true, "mech", "", "VEHICLE-LIGHTING-PATH-1: put GROUND VEHICLES on the same lighting path as every other object class.", passthrough),
L(NonEmpty, "MC2_VERIFY_MODE", Bool, false, "gameos", "", "", passthrough),
L(Presence, "MC2_VERTEX_PROJECT_FAST", Bool, false, "gameos", "", "", passthrough),
L(Presence, "MC2_VERTEX_PROJECT_PARITY", Bool, false, "gameos", "", "", passthrough),
// history: docs/gate-history.md#MC2_VFX_AGE_SAMPLE
L(Lead1, "MC2_VFX_AGE_SAMPLE", Bool, false, "code", "", "VFX-AGE-SAMPLE-1: sample GPU-particle spec curves (color/alpha/size/UV) at the routed effect's real CPU-advanced normalized age (gosFX Effect::m_age..."),
L(Lead1, "MC2_VFX_BLACKBODY", Bool, false, "gameos", "", ""),
// history: docs/gate-history.md#MC2_VFX_DEBUG_MODE
P("MC2_VFX_DEBUG_MODE", Int, 0, "gameos", "", "VFX-DEBUG-VIEWS-1: GPU particle billboard fragment-shader debug-mode selector (particle_billboard.frag u_debugMode, uploaded by gos_particle_bridge).", passthrough),  // default not derivable from the tree
L(Lead1, "MC2_VFX_DISTORTION", Bool, false, "gameos", "", ""),
P("MC2_VFX_DISTORT_AMP", Float, 0.0, "gameos", "", ""),  // default not derivable from the tree
L(Lead1, "MC2_VFX_DISTORT_FIXTURE", Bool, false, "gameos", "", ""),
// history: docs/gate-history.md#MC2_VFX_GPU_SIM_CARDCLOUD
L(Lead1, "MC2_VFX_GPU_SIM_CARDCLOUD", Bool, false, "code", "", "VFX-GPU-SIM-CARDCLOUD-BUFFER-1 (Stage 2 of the originals-restoration arc; docs/vfx-gpu-sim-spec.md)."),
// history: docs/gate-history.md#MC2_VFX_GPU_SIM_COMPARE
L(Lead1, "MC2_VFX_GPU_SIM_COMPARE", Bool, false, "code", "", "VFX-GPU-SIM-CARDCLOUD-BUFFER-1: emit the [VFX_GPU_SIM v1] integrity/compare logs (cpuActive vs submitted-live count, age/alpha ranges, world bounds, SSBO..."),
// history: docs/gate-history.md#MC2_VFX_LIT_PARTICLES
L(Lead1, "MC2_VFX_LIT_PARTICLES", Bool, false, "gameos", "", "VFX-LIT-PARTICLES-MVP-1 (Track V): simple directional+ambient lighting for alpha-blended smoke/dust particles.", passthrough),
// history: docs/gate-history.md#MC2_VFX_ORACLE_RENDER
L(Lead1, "MC2_VFX_ORACLE_RENDER", Bool, false, "code", "", "VFX-ORIGINAL-RECORD-ABI-1 (Phase 1 of the originals-restoration arc; see docs/vfx-originals-restoration-design.md).", passthrough),
L(Lead1, "MC2_VFX_ORACLE_SHAPE", Bool, false, "code", "", ""),
L(Lead1, "MC2_VFX_ORACLE_SHAPE_LOG", Bool, false, "unclassified", "", ""),
// history: docs/gate-history.md#MC2_VFX_ORACLE_TUBE
L(Lead1, "MC2_VFX_ORACLE_TUBE", Bool, false, "code", "", "GOSFX-TUBE-RIBBON-1: route supported gosFX Tube trails (MissileSmoke alpha, PPC additive, polygon) through the swept-quad ribbon oracle submit path instead..."),
// history: docs/gate-history.md#MC2_VFX_ORACLE_TUBE_COVERAGE
L(Lead1, "MC2_VFX_ORACLE_TUBE_COVERAGE", Bool, false, "gameos", "", "GOSFX-TUBE-RIBBON-1: wrap the deferred ribbon draw in a GL_SAMPLES_PASSED occlusion query; logs [VFX_ORACLE_TUBE coverage] ribbons/samples/fbo/sceneDrawBufs..."),
// history: docs/gate-history.md#MC2_VFX_ORACLE_TUBE_LOG
L(Lead1, "MC2_VFX_ORACLE_TUBE_LOG", Bool, false, "code", "", "GOSFX-TUBE-RIBBON-1: per-Tube ribbon submit counters (FIRST_HARVEST / v2 vertex+index tallies, unsupported->MLR fallbacks)."),
L(Lead1, "MC2_VFX_SCENECOLOR_GRAB", Bool, false, "gameos", "", ""),
// history: docs/gate-history.md#MC2_VFX_SOFT_PARTICLES
L(Lead1, "MC2_VFX_SOFT_PARTICLES", Bool, false, "gameos", "", "VFX-SOFT-PARTICLES-MVP-1 (Track V): depth-fade GPU particles at scene intersections.", passthrough),
L(Lead0, "MC2_VFX_TUBE_UPLOAD_STATS", Bool, false, "gameos", "", ""),
// history: docs/gate-history.md#MC2_VIEWMODE_FRAMEWORK
L(Lead0, "MC2_VIEWMODE_FRAMEWORK", Bool, false, "post", "", "VIEWMODE-POSTPROCESS-PRESENTATION-1: enables the view-mode presentation framework that wires ViewMode::ObjectIdDebug (and future sensor modes) through the...", passthrough),
// history: docs/gate-history.md#MC2_VIEWMODE_LOWLIGHT_GAIN
P("MC2_VIEWMODE_LOWLIGHT_GAIN", Float, 0.0, "post", "", "LOWLIGHT-NIGHTVISION-MVP-1: night-vision luminance amplification (clamped 0.1..16, default 2.5).", passthrough | range(0.1, 16.0)),  // default not derivable from the tree
// history: docs/gate-history.md#MC2_VIEWMODE_LOWLIGHT_TINT
L(NonEmpty, "MC2_VIEWMODE_LOWLIGHT_TINT", Bool, false, "post", "", "LOWLIGHT-NIGHTVISION-MVP-1: night-vision tint as 'r,g,b' floats (default 0.7,1.0,0.6 green-phosphor).", passthrough),
// history: docs/gate-history.md#MC2_VIEW_MODE
P("MC2_VIEW_MODE", Int, 0, "post", "", "VIEWMODE-POSTPROCESS-PRESENTATION-1: seeds the startup presentation mode (matches RenderCore::ViewMode).", passthrough),  // default not derivable from the tree
// history: docs/gate-history.md#MC2_VIEW_UNIFORMS
L(Lead0Kill, "MC2_VIEW_UNIFORMS", Bool, true, "props", "", "ViewUniforms UBO at binding=3.", passthrough),
L(Presence, "MC2_VISUAL_BOOKMARK_CAPTURE", Bool, false, "vk", "", "", passthrough),
L(Lead0, "MC2_VISUAL_CAPTURE_CLEAN", Bool, false, "gameos", "", ""),
L(Presence, "MC2_VISUAL_CAPTURE_DIR", Bool, false, "gameos", "", "", passthrough),
L(Presence, "MC2_VISUAL_CAPTURE_EXIT_AFTER", Bool, false, "gameos", "", "VK-NOINTEROP-STRICT-REVIEW: opt-in clean shutdown after the sweep so the VK island destroy lines (forbidden_* counters) + the final [VK_PRESENT] logHealth..."),
P("MC2_VISUAL_CAPTURE_FRAME", Int, -1, "vk", "", "", passthrough),
// history: docs/gate-history.md#MC2_VISUAL_CAPTURE_ON_FIRE
L(Presence, "MC2_VISUAL_CAPTURE_ON_FIRE", Bool, false, "unclassified", "", "VK-FIELD-VFX-8: GL twin of MC2_VK_PRESENT_DUMP_ON_FIRE."),
L(Lead0, "MC2_VISUAL_CAPTURE_POST", Bool, false, "gameos", "", "VK-PARITY-F2: MC2_VISUAL_CAPTURE_POST=1 (default OFF = byte-identical) reads the composited back buffer (post exposure/sunset grade, what the player sees)..."),
L(Lead0, "MC2_VISUAL_DIFF_CAPTURE", Bool, false, "gameos", "", ""),
P("MC2_VISUAL_DIFF_FRAME_N", Int, 90, "gameos", "", ""),
P("MC2_VISUAL_DIFF_MAX_FRAMES", Int, 0, "gameos", "", ""),  // default not derivable from the tree
G("MC2_VISUAL_DIFF_MISSION", Bool, false, "gameos", "", ""),  // default not derivable from the tree
G("MC2_VISUAL_DIFF_OUT", Bool, false, "gameos", "", ""),
P("MC2_VISUAL_SETTLE", Int, 0, "gameos", "", "", passthrough),  // default not derivable from the tree
G("MC2_VISUAL_TUNING_FILE", Bool, false, "gameos", "", "", passthrough),  // default not derivable from the tree
L(Lead0Kill, "MC2_VIS_SEMANTICS", Bool, true, "code", "", "ONE gate."),
L(Lead0, "MC2_VIS_SEMANTICS_TRACE", Bool, false, "code", "", ""),
P("MC2_VIS_SEMANTICS_TRACE_EVERY", Int, 120, "code", "", "", passthrough),
// history: docs/gate-history.md#MC2_VK_CSM_ARRAY_INTEROP
L(Lead1, "MC2_VK_CSM_ARRAY_INTEROP", Bool, false, "vk", "", "VK-CSM-ARRAY-DIRECT-1: presence-gated =1, default OFF.", passthrough),
// history: docs/gate-history.md#MC2_VK_CSM_FULLMAP_INTEROP
L(Lead1, "MC2_VK_CSM_FULLMAP_INTEROP", Bool, false, "vk", "", "VK-CSM-FULLMAP-DIRECT-1: presence-gated =1, default OFF.", passthrough),
// history: docs/gate-history.md#MC2_VK_DECAL_STATIC_GL_CANARY
G("MC2_VK_DECAL_STATIC_GL_CANARY", Bool, false, "gameos", "", "VK final-gate diagnostic: wraps gosRenderer::drawDecalStaticBatch GL draw loop in GL_ANY_SAMPLES_PASSED and logs [VK_SCENE_GL_DRAW_CANARY v1]..."),
G("MC2_VK_FEED_DUMP", String, "", "vk", "", ""),
// history: docs/gate-history.md#MC2_VK_FORCE_FAIL
L(Lead0, "MC2_VK_FORCE_FAIL", Bool, false, "vk", "", "VK-P13-SELECTOR-1: presence-gated =1, default OFF.", passthrough),
L(Presence, "MC2_VK_GPU_TIMESTAMPS", Bool, false, "vk", "", "", passthrough),
G("MC2_VK_HUD", Tri, false, "gameos", "", "VK-FIELD-ENV-CONTENT-STACK-ARM-1 (2026-07-11): the game HUD is UI CONTENT, and post- VK-RUNG8-ITEM8 the native VK composite is the sole present-image..."),
G("MC2_VK_HUD_COLOR_PROOF", Tri, false, "vk", "", "VK-FIELD-ENV-CONTENT-STACK-ARM-1 (2026-07-11): despite the legacy \"COLOR_PROOF\" name this is the ACTUAL native VK HUD renderer --..."),
L(Presence, "MC2_VK_HUD_DRAW_DUMP", Bool, false, "gameos", "", "Env-token-only memo (no disarm interaction): the env block is process-static, so one getenv per process is exact."),
G("MC2_VK_HUD_SKIP_GL", Tri, false, "gameos", "", "VK-FIELD-ENV-CONTENT-STACK-ARM-1: arm from the umbrella too."),
// history: docs/gate-history.md#MC2_VK_IMGUI
L(Lead1, "MC2_VK_IMGUI", Bool, false, "code", "", "VK-IMGUI-NATIVE-1 (rung 8.6): default-OFF ImGui Vulkan renderer gate.", passthrough),
L(Lead0Kill, "MC2_VK_LIGHTFEED_DEDUP", Bool, true, "vk", "", ""),
L(Exact1, "MC2_VK_MECH_FEED_AUDIT", Bool, false, "vk", "", ""),
L(Is01, "MC2_VK_MECH_FEED_NATIVE", Bool, false, "mech", "", ""),
L(Lead0Kill, "MC2_VK_MECH_FENCE_DEFER", Bool, true, "vk", "", ""),
// history: docs/gate-history.md#MC2_VK_MECH_LIGHTING
G("MC2_VK_MECH_LIGHTING", Tri, false, "vk", "", "VK-MECH-MATERIAL-LIGHTING-1: presence-gated =1, default OFF; REQUIRES MC2_VK_MECH_TIER1 (visible only with MC2_VK_MECH_MATERIAL=1).", passthrough),
// history: docs/gate-history.md#MC2_VK_MECH_MATERIAL
G("MC2_VK_MECH_MATERIAL", Tri, false, "vk", "", "VK-MECH-MATERIAL: presence-gated =1, default OFF (requires MC2_VK_MECH_TIER1).", passthrough),
L(Lead1, "MC2_VK_MECH_PERF", Bool, false, "vk", "", "", passthrough),
// history: docs/gate-history.md#MC2_VK_MECH_TIER1
G("MC2_VK_MECH_TIER1", Tri, false, "vk", "", "VK-MECH-TIER1: presence-gated =1, default OFF (requires MC2_VK_SHARED_SCENE_DEPTH).", passthrough),
// history: docs/gate-history.md#MC2_VK_NO_GL_INTEROP_RESOURCE_STRICT
G("MC2_VK_NO_GL_INTEROP_RESOURCE_STRICT", Bool, false, "vk", "", "VK Metal-gate resource canary.", passthrough),
// history: docs/gate-history.md#MC2_VK_NO_GL_INTEROP_STRICT
L(Lead1, "MC2_VK_NO_GL_INTEROP_STRICT", Bool, false, "vk", "", "VK Metal-gate canary.", passthrough),
L(Lead1, "MC2_VK_OBJECTID_DIRECT_CHAIN", Bool, false, "vk", "", "", passthrough),
// history: docs/gate-history.md#MC2_VK_PICK_PROBE
L(Presence, "MC2_VK_PICK_PROBE", Bool, false, "code", "", "BUG-A pick regression fence: default-OFF.", passthrough),
G("MC2_VK_POST_COMPOSITE", Tri, false, "vk", "", "VK-FIELD-ENV-COMPOSITE-ARM-1: arm from the umbrella (see directFboGateEnabled)."),
// history: docs/gate-history.md#MC2_VK_POST_COMPOSITE_DIRECT_CHAIN
G("MC2_VK_POST_COMPOSITE_DIRECT_CHAIN", Tri, false, "vk", "", "VK-POST-COMPOSITE-DIRECT-CHAIN: opt-in deletion-readiness proof gate, default OFF.", passthrough),
G("MC2_VK_POST_FOG_NATIVE", Tri, false, "vk", "", "VK-FIELD-ENV-CONTENT-STACK-ARM-1 (2026-07-11): now an umbrella family member (subGate)."),
L(Exact1, "MC2_VK_POST_HZB", Bool, false, "vk", "", ""),
L(Exact1, "MC2_VK_POST_SCREENFX", Bool, false, "vk", "", ""),
L(Lead0Kill, "MC2_VK_POST_SCREENFX_CLOUD", Bool, true, "vk", "", ""),
L(Lead0Kill, "MC2_VK_POST_SCREENFX_SHADOW", Bool, true, "vk", "", ""),
L(Lead0Kill, "MC2_VK_POST_SCREENFX_SHORELINE", Bool, true, "vk", "", ""),
L(Exact1, "MC2_VK_POST_SSAO", Bool, false, "vk", "", ""),
G("MC2_VK_PRESENT_DIRECT_FBO", Tri, false, "vk", "", "VK-FIELD-ENV-COMPOSITE-ARM-1: after VK-RUNG8-ITEM8 deleted the GL copy-1 present FBO, the native VK composite is the ONLY producer of the present image for..."),
// history: docs/gate-history.md#MC2_VK_PRESENT_DUMP_FRAME
P("MC2_VK_PRESENT_DUMP_FRAME", Int, 0, "vk", "", "VK-FIELD-VFX-3: frame pin for MC2_VK_PRESENT_DUMP_PPM."),
// history: docs/gate-history.md#MC2_VK_PRESENT_DUMP_ON_FIRE
L(Presence, "MC2_VK_PRESENT_DUMP_ON_FIRE", Bool, false, "vk", "", "VK-FIELD-VFX-5: dynamic fire-event pin for MC2_VK_PRESENT_DUMP_PPM."),
// history: docs/gate-history.md#MC2_VK_PRESENT_DUMP_PPM
G("MC2_VK_PRESENT_DUMP_PPM", Bool, false, "vk", "", "VK-FIELD-ORACLE-1: headless present-pixel oracle."),
// history: docs/gate-history.md#MC2_VK_PRESENT_NATIVE_STRICT
L(Lead1, "MC2_VK_PRESENT_NATIVE_STRICT", Bool, false, "vk", "", "VK present-tail deletion-readiness canary.", passthrough),
// history: docs/gate-history.md#MC2_VK_PRESENT_NO_GL_IMPORT
L(Lead1, "MC2_VK_PRESENT_NO_GL_IMPORT", Bool, false, "vk", "", "VK present-tail ownership gate.", passthrough),
L(Lead1, "MC2_VK_PRESENT_SKIP_GL_HANDOFF", Bool, false, "vk", "", "", passthrough),
// history: docs/gate-history.md#MC2_VK_PRESENT_TAIL_HOOK
G("MC2_VK_PRESENT_TAIL_HOOK", Tri, false, "vk", "", "VK-TAIL-HOOK-1 (rung 8.6): default-OFF present-command-buffer seam for native final-tail rendering.", passthrough),
L(Presence, "MC2_VK_PRESENT_VERIFY", Bool, false, "gameos", "", "", passthrough),
// history: docs/gate-history.md#MC2_VK_PROP_COLOR_TIER1
G("MC2_VK_PROP_COLOR_TIER1", Tri, false, "vk", "", "VK-P11 TIER-1 PROP COLOR: presence-gated =1, default OFF; REQUIRES MC2_VK_SHARED_SCENE_DEPTH.", passthrough),
// history: docs/gate-history.md#MC2_VK_PROP_DIRECT_CHAIN
G("MC2_VK_PROP_DIRECT_CHAIN", Tri, false, "vk", "", "VK-PROP-DIRECT-CHAIN: opt-in deletion-readiness proof gate, default OFF.", passthrough),
L(Lead0Kill, "MC2_VK_PROP_FENCE_DEFER", Bool, true, "vk", "", ""),
L(Presence, "MC2_VK_PROP_LIGHT_PROBE", Bool, false, "vk", "", ""),
L(Presence, "MC2_VK_PROP_LIGHT_VERTEX_PROBE", Bool, false, "vk", "", ""),
// history: docs/gate-history.md#MC2_VK_PROP_MATERIAL
G("MC2_VK_PROP_MATERIAL", Tri, false, "vk", "", "VK-PROP-MATERIAL: presence-gated =1, default OFF (requires MC2_VK_PROP_COLOR_TIER1).", passthrough),
// history: docs/gate-history.md#MC2_VK_PROP_MATERIAL_DEBUG
L(Presence, "MC2_VK_PROP_MATERIAL_DEBUG", Bool, false, "props", "", "VK-PROP-MATERIAL-DEBUG: presence-gated =1, default OFF.", passthrough),
L(Presence, "MC2_VK_PROP_MIP_PROBE", Bool, false, "vk", "", "ADJUDICATION-1 FENCE-RECON: one-shot mip-count-parity verdict."),
L(NonEmpty, "MC2_VK_PROP_PACKET_DUMP", Bool, false, "vk", "", ""),
L(Lead1, "MC2_VK_PROP_PERF", Bool, false, "vk", "", "", passthrough),
// history: docs/gate-history.md#MC2_VK_SCENE_GL_PRODUCER_QUERY
L(Lead1, "MC2_VK_SCENE_GL_PRODUCER_QUERY", Bool, false, "post", "", "VK final-gate diagnostic: wraps the sceneFBO producer phase in a GL_ANY_SAMPLES_PASSED query and logs [VK_SCENE_GL_PRODUCER_QUERY v1] any_samples=... so..."),
// history: docs/gate-history.md#MC2_VK_SCREENSHADOW_GL_CANARY
L(Lead1, "MC2_VK_SCREENSHADOW_GL_CANARY", Bool, false, "post", "", "VK final-gate diagnostic: wraps only the legacy GL runScreenShadow fullscreen draw in GL_ANY_SAMPLES_PASSED and logs [VK_SCREENSHADOW_GL_CANARY v1]."),
L(Lead1, "MC2_VK_SELECT_TEST", Bool, false, "code", "", ""),
// history: docs/gate-history.md#MC2_VK_SHADOW_INTEROP
L(Lead1, "MC2_VK_SHADOW_INTEROP", Bool, false, "vk", "", "VK-P8a SHADOW-INTEROP: presence-gated =1, default OFF.", passthrough),
// history: docs/gate-history.md#MC2_VK_SHADOW_MATRIX_DIAG
L(Lead0, "MC2_VK_SHADOW_MATRIX_DIAG", Bool, false, "vk", "", "VK shadow producer/consumer evidence (default OFF = zero cost; GameOS/gameos/vulkan_shadow_island_live.cpp).", passthrough),
// history: docs/gate-history.md#MC2_VK_SHARED_GBUFFER
L(Exact1, "MC2_VK_SHARED_GBUFFER", Bool, false, "vk", "", "VK-P14 GATE-CONSOLIDATION-1: presence-gated =1, default OFF.", passthrough),
// history: docs/gate-history.md#MC2_VK_SHARED_SCENE_DEPTH
G("MC2_VK_SHARED_SCENE_DEPTH", Tri, false, "vk", "", "VK-TIER1 SHARED SCENE DEPTH: presence-gated =1, default OFF.", passthrough),
// history: docs/gate-history.md#MC2_VK_SHARED_SCENE_STRICT
L(Exact1, "MC2_VK_SHARED_SCENE_STRICT", Bool, false, "post", "", "VK shared-scene deletion-readiness canary.", passthrough),
// history: docs/gate-history.md#MC2_VK_STATIC_SHADOW_MAP
L(Is01, "MC2_VK_STATIC_SHADOW_MAP", Bool, false, "post", "", "DEPRECATED no-op (VK-RUNG8-ITEM6).", passthrough),
G("MC2_VK_TERRAIN_ALL_LODS", Tri, false, "vk", "", ""),
L(Presence, "MC2_VK_TERRAIN_COPY_STATS", Bool, false, "vk", "", "", passthrough),
// history: docs/gate-history.md#MC2_VK_TERRAIN_CSM
G("MC2_VK_TERRAIN_CSM", Tri, false, "terrain", "", "VK-TERRAIN-CSM (biggest-safe-subset): presence-gated =1, default OFF (requires MC2_VK_TERRAIN_DYNSHADOW chain + CSM active).", passthrough),
P("MC2_VK_TERRAIN_CSM_DEBUG", Int, 0, "vk", "", ""),
L(Lead1, "MC2_VK_TERRAIN_CSM_FULLMAP_EXPERIMENTAL", Bool, false, "terrain", "", ""),
// history: docs/gate-history.md#MC2_VK_TERRAIN_DECAL_GL_CANARY
G("MC2_VK_TERRAIN_DECAL_GL_CANARY", Bool, false, "gameos", "", "VK final-gate diagnostic: wraps gosRenderer::drawDecals GL draw loop in GL_ANY_SAMPLES_PASSED and logs [VK_SCENE_GL_DRAW_CANARY v1] tag=terrain_decal_batch."),
// history: docs/gate-history.md#MC2_VK_TERRAIN_DETAIL
G("MC2_VK_TERRAIN_DETAIL", Tri, false, "terrain", "", "VK-TERRAIN-DETAIL: presence-gated =1, default OFF (requires MC2_VK_TERRAIN_LIGHTING).", passthrough),
// history: docs/gate-history.md#MC2_VK_TERRAIN_DYNSHADOW
G("MC2_VK_TERRAIN_DYNSHADOW", Tri, false, "terrain", "", "VK-TERRAIN-DYNSHADOW: presence-gated =1, default OFF (requires MC2_VK_TERRAIN_LIGHTING).", passthrough),
L(Lead0Kill, "MC2_VK_TERRAIN_FENCE_DEFER", Bool, true, "vk", "", ""),
// history: docs/gate-history.md#MC2_VK_TERRAIN_GL_FALLBACK_AUDIT
L(Exact1, "MC2_VK_TERRAIN_GL_FALLBACK_AUDIT", Bool, false, "terrain", "", "VK terrain ownership diagnostic: under the max-native terrain gates, counts terrain chunks captured by the Vulkan feed vs chunks that still fall through to..."),
// history: docs/gate-history.md#MC2_VK_TERRAIN_HEIGHT_AUDIT
L(Exact1, "MC2_VK_TERRAIN_HEIGHT_AUDIT", Bool, false, "vk", "", "VK-TERRAIN HEIGHT-AUDIT (verification-only, default OFF; requires MC2_VK_TERRAIN_TIER1).", passthrough),
// history: docs/gate-history.md#MC2_VK_TERRAIN_HEIGHT_AUDIT_CORRUPT
L(Exact1, "MC2_VK_TERRAIN_HEIGHT_AUDIT_CORRUPT", Bool, false, "vk", "", "VK-TERRAIN HEIGHT-AUDIT NEGATIVE CONTROL (test-only, default OFF; requires MC2_VK_TERRAIN_HEIGHT_AUDIT).", passthrough),
// history: docs/gate-history.md#MC2_VK_TERRAIN_LIGHTING
G("MC2_VK_TERRAIN_LIGHTING", Tri, false, "terrain", "", "VK-TERRAIN-LIGHTING: presence-gated =1, default OFF (requires MC2_VK_TERRAIN_TIER1).", passthrough),
// history: docs/gate-history.md#MC2_VK_TERRAIN_LODFADE
L(Exact1, "MC2_VK_TERRAIN_LODFADE", Bool, false, "terrain", "", "VK-TERRAIN-LODFADE: presence-gated =1, default OFF (requires MC2_VK_TERRAIN_DETAIL).", passthrough),
// history: docs/gate-history.md#MC2_VK_TERRAIN_OVERLAY_GL_CANARY
G("MC2_VK_TERRAIN_OVERLAY_GL_CANARY", Bool, false, "gameos", "", "VK final-gate diagnostic: wraps gosRenderer::drawTerrainOverlays GL draw loop in GL_ANY_SAMPLES_PASSED and logs [VK_SCENE_GL_DRAW_CANARY v1]..."),
L(Lead1, "MC2_VK_TERRAIN_PERF", Bool, false, "vk", "", "", passthrough),
G("MC2_VK_TERRAIN_SKIRTS", Tri, false, "vk", "", ""),
// history: docs/gate-history.md#MC2_VK_TERRAIN_TIER1
G("MC2_VK_TERRAIN_TIER1", Tri, false, "vk", "", "VK-P10 TERRAIN TIER-1: presence-gated =1, default OFF (requires MC2_VK_SHARED_SCENE_DEPTH).", passthrough),
L(Exact1, "MC2_VK_TERRAIN_VISUALDISPLACE_FAR", Bool, false, "vk", "", ""),
// history: docs/gate-history.md#MC2_VK_VFX
G("MC2_VK_VFX", Tri, false, "vk", "", "VK-FIELD-VFX-1: umbrella sub-gate, default-ON under MC2_VK_SHARED_GBUFFER (explicit =0 forces off, =1 forces on).", passthrough),
// history: docs/gate-history.md#MC2_VK_VFX_COALESCE
L(Lead0Kill, "MC2_VK_VFX_COALESCE", Bool, true, "gameos", "", "VK-VFX-COALESCE-1: merges consecutive same-render-state HUD tri-batch draws (gosRenderer::drawTris IsHUD branch, gameos_graphics.cpp) into ONE hudBatch_...", passthrough),
// history: docs/gate-history.md#MC2_VK_VFX_TRACE
L(Lead0, "MC2_VK_VFX_TRACE", Bool, false, "vk", "", "VK-FIELD-VFX-2: per-bolt weapon-fire pipeline trace."),
// history: docs/gate-history.md#MC2_VK_WATER_DIRECT_CHAIN
G("MC2_VK_WATER_DIRECT_CHAIN", Tri, false, "vk", "", "VK-WATER-DIRECT-CHAIN: opt-in deletion-readiness proof gate, default OFF.", passthrough),
L(Lead0Kill, "MC2_VK_WATER_FENCE_DEFER", Bool, true, "vk", "", ""),
L(Lead1, "MC2_VK_WATER_PERF", Bool, false, "vk", "", "", passthrough),
G("MC2_VK_WATER_TIER1", Tri, false, "vk", "", "Direct-chain members default ON under the umbrella; MC2_VK_WATER_TIER1 is a normal umbrella sub-gate."),
L(Presence, "MC2_VPL_CULL", Bool, false, "terrain", "", "VPL retirement: the MC2_VPL_CULL / MC2_VPL_REDUCE getenv reads are KEPT solely to gate the one-shot event=retired lifecycle lines emitted just before the..."),
L(Presence, "MC2_VPL_PICK", Bool, false, "code", "", "VPL-retirement Step 3 (3a): recursion-free tile selection."),
L(Presence, "MC2_VPL_REDUCE", Bool, false, "terrain", "", ""),
L(Lead1, "MC2_VSYNC", Bool, false, "vk", "", "", passthrough),
G("MC2_VULKAN_SPV_DIR", Bool, false, "vk", "", "", passthrough),  // default not derivable from the tree
G("MC2_VULKAN_VALIDATION", Bool, false, "vk", "", "", passthrough),
L(Presence, "MC2_WATCHID_GENERATION", Bool, false, "code", "", "MF3-GENERATIONAL-HANDLE-1: generational watch-id validation killswitch.", passthrough),
L(Presence, "MC2_WATER_ASPECT_DIAG", Bool, false, "code", "", "[WATER_ASPECT_DIAG v1] env-gated one-shot (MC2_WATER_ASPECT_DIAG=1), silent by default."),
L(Lead0, "MC2_WATER_BIAS_LEGACY", Bool, false, "gameos", "", ""),
L(Presence, "MC2_WATER_DEBUG", Bool, false, "terrain", "", "MC2_WATER_DEBUG=1: post-warmup population recon for the renderWater slice.", passthrough),
P("MC2_WATER_DEBUG_MODE", Int, 0, "gameos", "", ""),
L(Presence, "MC2_WATER_DEPTHPROBE", Bool, false, "terrain", "", "[WATER_DEPTHPROBE v1] RETAINED verification instrument (env-gated MC2_WATER_DEPTHPROBE, silent by default; demote-not-delete per the debug-instrumentation...", passthrough),
L(Lead0Kill, "MC2_WATER_EDGE_FEATHER", Bool, true, "gameos", "", ""),
P("MC2_WATER_EDGE_FEATHER_STRENGTH", Float, 1.0, "gameos", "", ""),
L(Presence, "MC2_WATER_FULL_RECIPE_SPIKE", Bool, false, "terrain", "", "WATER-FULL-RECIPE-GPU-TIMER-SPIKE-0 (MC2_WATER_FULL_RECIPE_SPIKE): measurement only."),
L(Presence, "MC2_WATER_GATE_DIAG", Bool, false, "terrain", "", "S2.15 gate diag: editor fails to arm FAST water path - print once per (mission/state-change) what gate fails.", passthrough),
L(Lead0Kill, "MC2_WATER_GPU_FULL_RECIPE_AUTHORITATIVE", Bool, true, "terrain", "", ""),
L(Lead0P, "MC2_WATER_GPU_FULL_RECIPE_CULL", Bool, false, "terrain", "", "WATER-GPU-FULL-RECIPE-CULL-1A (MC2_WATER_GPU_FULL_RECIPE_CULL, default-OFF): runs the full-recipe cull path NON-AUTHORITATIVELY (candidate path still draws..."),
P("MC2_WATER_HDRI_LOD", Float, 0.0, "gameos", "", "", passthrough),  // default not derivable from the tree
L(Lead0, "MC2_WATER_HDRI_REFL_FULL", Bool, false, "gameos", "", "", passthrough),
L(Presence, "MC2_WATER_MATERIAL_PROBE", Bool, false, "terrain", "", ""),
L(Presence, "MC2_WATER_NO_DEPTH_WRITE", Bool, false, "gameos", "", ""),
L(Lead0Kill, "MC2_WATER_REFLECTION", Bool, true, "gameos", "", ""),
L(Lead0, "MC2_WATER_REFLECTION_RT", Bool, false, "terrain", "", "", passthrough),
L(Lead0, "MC2_WATER_REFL_RT_PIXELPROOF", Bool, false, "terrain", "", "", passthrough),
L(Presence, "MC2_WATER_RENDERPROBE", Bool, false, "terrain", "", "[WATER_RENDERPROBE v1] Fix B matrix-share release gate.", passthrough),
L(Presence, "MC2_WATER_S6_COST", Bool, false, "terrain", "", "S6 coarse cost A/B instrument: ONE QPC pair around the WHOLE per-frame setupTextures loop (NOT per-quad - the per-quad std::chrono COST_SPLIT scopes are...", passthrough),
L(Presence, "MC2_WATER_S6_TRACE", Bool, false, "terrain", "", ""),
// history: docs/gate-history.md#MC2_WATER_SHINE
L(Lead0, "MC2_WATER_SHINE", Bool, false, "gameos", "", "[DEFAULT-OFF OPT-IN.", passthrough),
// history: docs/gate-history.md#MC2_WATER_SHORE3_DEPTH_K
P("MC2_WATER_SHORE3_DEPTH_K", Float, 0.03, "gameos", "", "Shore mode 3 tuning: u_shoreDepthK, fraction of clip.z added to the flat water plane depth (depth-only lift so coarse terrain triangles do not carve...", passthrough),
// history: docs/gate-history.md#MC2_WATER_SHORE3_EXTRA_WU
P("MC2_WATER_SHORE3_EXTRA_WU", Float, 60.0, "terrain", "", "Shore mode 3 (MC2_WATER_SHORE_MODE=3) tuning: extra world units added to the water admission extent beyond the sidecar contour so geometry exists for the...", passthrough),
P("MC2_WATER_SHORE_FEATHER", Float, 0.0, "terrain", "", "", passthrough),
P("MC2_WATER_SHORE_FEATHER_WU", Float, 7.0, "gameos", "", ""),
// history: docs/gate-history.md#MC2_WATER_SHORE_MASK
G("MC2_WATER_SHORE_MASK", Bool, true, "terrain", "", "WATER-SHORE-MASK-1: per-mission shoreline mask sidecar (data/missions/<m>.beauty/shoreline_mask.png) drives the water shore edge.", passthrough),
// history: docs/gate-history.md#MC2_WATER_SHORE_MEANDER
P("MC2_WATER_SHORE_MEANDER", Float, 0.0, "terrain", "", "SHORELINE-GEOMETRY-1: fBm meander amplitude in world units applied to the shore contour in gos_terrain_water_stream.cpp ShoreMeanderWu(); breaks the...", passthrough),
// history: docs/gate-history.md#MC2_WATER_SHORE_MEANDER_FREQ
P("MC2_WATER_SHORE_MEANDER_FREQ", Float, 0.00260417, "terrain", "", "SHORELINE-GEOMETRY-1 companion tuning knob for MC2_WATER_SHORE_MEANDER: spatial frequency of the shore-contour fBm, in cycles per world unit...", passthrough),
G("MC2_WATER_SHORE_MODE", Enum, "", "terrain", "", "", tokens("|0|1|2|3")),
L(Lead0, "MC2_WATER_SKYTINT", Bool, false, "gameos", "", ""),
L(Presence, "MC2_WATER_STREAM_DEBUG", Bool, false, "terrain", "", "", passthrough),
L(Lead1, "MC2_WATER_THINRING_TRACE", Bool, false, "terrain", "", "", passthrough),
L(Lead0Kill, "MC2_WATER_UPLOAD_NARROW", Bool, true, "terrain", "", ""),
P("MC2_WIDTH", Int, 0, "gameos", "", "(c) #49 windowed-resolution oracle.", passthrough),
L(Lead0, "MC2_WINDOWED", Bool, false, "gameos", "", "", passthrough),
// history: docs/gate-history.md#MC2_WINDOW_OFFSCREEN
L(Lead0, "MC2_WINDOW_OFFSCREEN", Bool, false, "unclassified", "", "NOFOCUS-HARDEN-1: create the SDL window HIDDEN at (-32000,-32000) and keep it there, so an agent-launched engine window never occupies visible desktop area...", passthrough),
L(Lead0Kill, "MC2_WINDOW_RESIZABLE", Bool, true, "gameos", "", ""),
// history: docs/gate-history.md#MC2_XFORM_PARITY_FATAL
L(Lead1, "MC2_XFORM_PARITY_FATAL", Bool, false, "code", "", "XFORM-CONVENTION-HARNESS-1: promote the F1-3C clip-space parity probe (code/gamecam.cpp) to a HARD invariant."),
};

constexpr std::size_t kCount = sizeof(kTable) / sizeof(kTable[0]);

static_assert(nameIsSortedUnique(kTable, kCount), "gate_table.cpp: rows must be sorted by name with no duplicates");
static_assert(firstInsaneRow(kTable, kCount) == kCount, "gate_table.cpp: a row breaks a table rule (see rowIsSane in gate_table_dsl.h)");

#ifndef GATE_INDEX_BOOTSTRAP
constexpr bool matchesIndex() {
    if (kCount != mc2::gate::detail::kGateCount) return false;
    for (std::size_t i = 0; i < kCount; ++i) {
        const Row& r = kTable[i];
        const IndexEntry& e = mc2::gate::detail::kIndex[i];
        if (r.name != e.name || r.kind != e.kind || r.rule != e.rule || r.tokens != e.tokens) return false;
    }
    return true;
}
static_assert(matchesIndex(), "gate_table.cpp and gate_index.gen.h disagree: run py -3 tools/gate_gen.py --write");
#endif

}  // namespace mc2::gate::rows::tbl

namespace mc2::gate {
const Row* tableData() noexcept { return rows::tbl::kTable; }
std::size_t tableSize() noexcept { return rows::tbl::kCount; }
}  // namespace mc2::gate
