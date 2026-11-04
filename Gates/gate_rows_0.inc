L(Lead1, "MC2_ABL_ARG_GUARD", Bool, false, "code", "", "ABL crash-safety gates (ABL-OPCODE-BOUNDS-HARDEN) MC2_ABL_ARG_GUARD=1 - enables dispatch-bounds soft path + out-ptr null guards MC2_ABL_RUNTIME_SOFTFAIL=1 -...", passthrough | verified),
L(Lead1, "MC2_ABL_ARG_GUARD_REPRO", Bool, false, "code", "", "", passthrough | verified),
L(Lead0Kill, "MC2_ABL_COMPILE_FATAL", Bool, true, "code", "", "ASAN-3-ABL-COMPILE-FATAL-1: fail at preprocess when ABL syntax errors leave partial bytecode; set =0 only for legacy execute-anyway diagnostics.", passthrough | registry(Feature, "MC2_ABL_COMPILE_FATAL", -1, true)),
G("MC2_ABL_COMPILE_FATAL_REPRO", String, "", "code", "", "ASAN-3-ABL-COMPILE-FATAL-1: preprocess the supplied malformed-fixture path during ABL init to prove fail-loud behavior.", passthrough | registry(Trace, "MC2_ABL_COMPILE_FATAL_REPRO", -1, false) | verified),
L(AtoiNZ, "MC2_ABL_CORE_TRACE", Bool, false, "code", "", "", verified),
L(Presence, "MC2_ABL_REG_TRACE", Bool, false, "unclassified", "", "", verified),
L(Lead1, "MC2_ABL_RUNTIME_SOFTFAIL", Bool, false, "code", "", "", passthrough | verified),
L(Presence, "MC2_ABL_SKIP_ERRORED_MODULES", Bool, false, "code", "", "", passthrough | verified),
L(Presence, "MC2_ABL_TRACE", Bool, false, "code", "", "", verified),
L(Lead0, "MC2_ABL_VM_FACTS", Bool, false, "code", "", "", verified),
G("MC2_ACTIVE_MOD", String, "", "code", "", "", reread | passthrough | verified),
// history: docs/gate-history.md#MC2_AIMED_FIRE_FIXTURE
L(Lead0P, "MC2_AIMED_FIRE_FIXTURE", Bool, false, "code", "", "ISSUE-9 headless probe (code/warrior.cpp MechWarrior::updateActions): every home-team mech without a combat order is issued an aimed-HEAD attack...", passthrough | registry(Trace, "MC2_TRACE_AIMED_FIRE_FIXTURE", -1, false) | verified),
L(Presence, "MC2_ALPHA_TEST_TRACE", Bool, false, "props", "", "", verified),
// -------------------------------------------------------------------
// ENV-REGISTRY-LEGACY-SWEEP-1: promotions from the legacy-var registry
// sweep (scripts/check-env-registry.sh ALLOWLIST). These are genuinely
// active feature gates pulled out of the allowlist into the real
// registry; see scripts/check-env-registry.sh history for the other
// ~355 vars that stayed allowlisted (trace/override/infra/parity/legacy).
// -------------------------------------------------------------------
L(Presence, "MC2_ANIM_CADENCE_GUARD", Bool, false, "mech", "", "", passthrough | verified),
// history: docs/gate-history.md#MC2_APPEAR_AUDIT
L(Lead0, "MC2_APPEAR_AUDIT", Bool, false, "code", "", "META-FIX design B PART 4 (code/mission.cpp Mission::start beside mission_phase_mark('mission_ready')): default OFF, OBSERVATIONAL-FIRST.", passthrough | registry(Trace, "MC2_TRACE_APPEAR_AUDIT", -1, false) | verified),
L(Lead1, "MC2_ASSET_SCALE_SELFTEST", Bool, false, "gameos", "", "", verified),
L(Lead1, "MC2_ASSET_SCALE_TRACE", Bool, false, "gameos", "", "", verified),
L(Lead0Kill, "MC2_ASSIMP_IMPORT", Bool, true, "code", "", "", verified),
// history: docs/gate-history.md#MC2_ASSIMP_MECH_IMPORT
L(Lead0Kill, "MC2_ASSIMP_MECH_IMPORT", Bool, true, "mech", "", "BT2018 mech import 1A: route mech geometry through the Assimp GLTF/FBX importer instead of the legacy MSL loader.", registry(Feature, "MC2_FEATURE_ASSIMP_MECH_IMPORT", -1, true) | verified),
// TRACKV-CI-GATE-RESTORE-1 diagnostic trace vars
L(Presence, "MC2_BLDG_DIAG_TRACE", Bool, false, "props", "", "", verified),
L(Presence, "MC2_BLDG_REG_TRACE", Bool, false, "props", "", "", verified),
L(Lead0Kill, "MC2_BLDG_TYPE_ANIM_STATIC_ELIGIBLE", Bool, true, "props", "", "", passthrough | verified),
L(Presence, "MC2_BLOCK_FRUSTUM_FALLBACK", Bool, false, "terrain", "", "F6 T1/T2: cache frustum planes once per frame (O(1)) into Camera member.", verified),
G("MC2_BOOKMARK_SAVE_DIR", String, "", "code", "", "Directory the Save Camera Bookmark button writes into; empty = ./saved_bookmarks.", passthrough | verified),
L(Lead1, "MC2_BOOKMARK_SAVE_TEST", Bool, false, "code", "", "BOOKMARK-SAVE-BUTTON-1 test hook: =1 triggers one bookmark save shortly after mission load.", passthrough | verified),
// history: docs/gate-history.md#MC2_BOOT_INFO
P("MC2_BOOT_INFO", Int, 0, "code", "", "INFO-CAPTURE fixture: =N force-selects the first home mover into the in-mission Info window and switches the command bar to the INFO tab ONCE at frame >= N...", registry(Trace, "MC2_TRACE_BOOT_INFO", -1, false) | verified),
// history: docs/gate-history.md#MC2_BOOT_INFO_SCROLL
L(Lead0, "MC2_BOOT_INFO_SCROLL", Bool, false, "code", "", "INFO-IMGUI-SLICE-4 capture assist: =1 (with MC2_BOOT_INFO=N driving) pins the info-panel loadout list scrolled to bottom from frame N+3000 on BOTH paths -...", registry(Trace, "MC2_TRACE_BOOT_INFO_SCROLL", -1, false) | verified),
// history: docs/gate-history.md#MC2_BOOT_OBJ
P("MC2_BOOT_OBJ", Int, 0, "code", "", "OBJ-CAPTURE fixture: =N calls ControlGui::startObjectives(1) ONCE at frame >= N (ControlGui::update), showing the in-mission objectives overlay.", registry(Trace, "MC2_TRACE_BOOT_OBJ", -1, false) | verified),
// history: docs/gate-history.md#MC2_BOOT_OBJ_COMPLETE
P("MC2_BOOT_OBJ_COMPLETE", Int, 0, "code", "", "OBJ-COMPLETE-CAPTURE fixture: =N force-resolves the first active non-hidden objective to OS_SUCCESSFUL ONCE at frame >= N (ControlGui::update) via the...", registry(Trace, "MC2_TRACE_BOOT_OBJ_COMPLETE", -1, false) | verified),
// history: docs/gate-history.md#MC2_BOOT_PAUSED
P("MC2_BOOT_PAUSED", Int, 0, "code", "", "PAUSE-IMGUI-SLICE-0: =N auto-toggles the in-mission pause menu ONCE at frame >= N (MissionInterfaceManager::update).", registry(Trace, "MC2_TRACE_BOOT_PAUSED", -1, false) | verified),
// history: docs/gate-history.md#MC2_BOOT_PAUSED_CONFIRM
G("MC2_BOOT_PAUSED_CONFIRM", String, "", "code", "", "PAUSE-IMGUI-SLICE-3: =abort|exit invokes the ABORT MISSION / EXIT GAME confirm dialog ONCE after the MC2_BOOT_PAUSED pause menu has settled...", registry(Trace, "MC2_TRACE_BOOT_PAUSED_CONFIRM", -1, false) | verified),
P("MC2_BOOT_QUICKLOAD_AT", Int, 0, "code", "", "", verified),
P("MC2_BOOT_QUICKSAVE_AT", Int, 0, "code", "", "", verified),
// history: docs/gate-history.md#MC2_BOOT_TIMER
P("MC2_BOOT_TIMER", Int, 0, "code", "", "TIMER-CAPTURE fixture: =N force-arms the in-mission mission timer ONCE at frame >= N by setting mission->m_timeLimit = scenarioTime + 754 s...", registry(Trace, "MC2_TRACE_BOOT_TIMER", -1, false) | verified),
// history: docs/gate-history.md#MC2_BOOT_TIMER_SECS
P("MC2_BOOT_TIMER_SECS", Float, 754.0, "code", "", "TIMER-CAPTURE fixture parameter: =S overrides the remaining seconds the MC2_BOOT_TIMER fixture arms (default 754).", rangeAbove(0.0, 3.4e38) | registry(Trace, "MC2_TRACE_BOOT_TIMER_SECS", -1, false) | verified),
G("MC2_BOOT_TO_BAY", String, "", "code", "", "Headless harness: campaign name to boot straight into (mech bay); a non-empty value also skips front-end videos.", passthrough | verified),
L(Presence, "MC2_BOOT_TO_MECHLAB", Bool, false, "mech", "", "", verified),
G("MC2_BOOT_TO_MISSION", String, "", "code", "", "MC2_BOOT_TO_MISSION=<mission FileName>: jump to a specific mission's logistics instead of the campaign's first mission.", passthrough | verified),
G("MC2_BOOT_TO_SCREEN", String, "", "mech", "", "Headless harness: logistics screen to park on when MC2_BOOT_TO_BAY is set (purchase|bay|loadout|launch|briefing; unset = bay).", passthrough | verified),
// history: docs/gate-history.md#MC2_BOOT_VIDEO
L(Lead0, "MC2_BOOT_VIDEO", Bool, false, "code", "", "VIDEO-CAPTURE fixture: =1 overrides the smoke-mode movie skip in MC2Movie::init so in-mission comm videos (mc2_01 V1A.bik ~frame 1600) play under...", registry(Trace, "MC2_TRACE_BOOT_VIDEO", -1, false) | passthrough | verified),
L(AtoiNZ, "MC2_BRAIN_FIXED_TICK", Bool, false, "code", "", "", verified),
P("MC2_BRAIN_FIXED_TICK_HZ", Float, 10.0, "code", "", "Brain fixed-tick rate in Hz; clamps to 1..30.", clampRange(1.0, 30.0) | verified),
L(AtoiNZ, "MC2_BRAIN_FIXED_TICK_TRACE", Bool, false, "code", "", "", verified),
// SAVE/LOAD-AIFREEZE-TRACE-1: runBrain post-exec diagnostic.
// history: docs/gate-history.md#MC2_BRAIN_FREEZE_TRACE
L(Lead0, "MC2_BRAIN_FREEZE_TRACE", Bool, false, "code", "", "SAVE/LOAD-AIFREEZE-TRACE-1 (+ ISSUE-10 extension): one-line stderr trace at end of MechWarrior::runBrain (brainState, curTacOrder.code, movePath.numSteps...", passthrough | registry(Trace, "MC2_BRAIN_FREEZE_TRACE", -1, false) | verified),
G("MC2_BRAIN_MISSION_SEED", String, "", "code", "", "Tactic-weights mission seed text (decimal, 0x hex, leading-0 octal; strtoul base 0 at the reader); empty = the built-in seed.", passthrough | verified),
G("MC2_BRAIN_POLICY", Enum, "", "code", "", "Brain execution policy override: legacy forces stock ABL, techbrain forces TechBrain; unset or auto decides per mission at load.", passthrough | tokens("|auto|legacy|techbrain") | verified),
G("MC2_BRAIN_RUNTIME_FORCE_MODE", Enum, "", "code", "", "MC2_BRAIN_RUNTIME_FORCE_MODE overrides all per-unit modes (post-load global override).", passthrough | tokens("|legacy|enhanced|hybrid") | verified),
L(AtoiNZ, "MC2_BRAIN_RUNTIME_TRACE", Bool, false, "code", "", "", passthrough | verified),
G("MC2_BRAIN_SPECIAL_FIT", String, "", "code", "", "DISPATCH-EFFECT-UNITEJECT-1: name of the _specials.fit every warrior loads instead of the mission's own (fixture override); empty = mission name.", passthrough | verified),
L(AtoiNZ, "MC2_BRAIN_TASKQ_TRACE", Bool, false, "code", "", "", passthrough | verified),
G("MC2_BRIDGE", Bool, false, "gameos", "", "Dev tool selector: harness-owned read-only socket; requires port and session GUID.", verified),
P("MC2_BRIDGE_MOVER_PERIOD_SEC", Float, 2.0, "code", "", "Seconds between bridge-mover state bursts; below 0.25 clamps to 0.25.", passthrough | clampRange(0.25, 3.4e38) | verified),
L(Presence, "MC2_BRIDGE_MOVER_STATE", Bool, false, "unclassified", "", "", passthrough | verified),
P("MC2_BRIDGE_PORT", Int, 0, "gameos", "", "Harness loopback connect-back port; zero selects no transport.", verified),
G("MC2_BRIDGE_SESSION", String, "", "gameos", "", "Fresh harness session GUID, echoed in the v1 hello.", verified),
L(Presence, "MC2_BUCKET_CENSUS", Bool, false, "terrain", "", ""),
// history: docs/gate-history.md#MC2_BURNIN_NO_JPG
// GitHub-issue fix batch 2026-07-16, part 2 (issues #9, #17)
L(Lead0P, "MC2_CALLED_SHOT_FIX", Bool, false, "mech", "", "ISSUE-9 called-shot effectiveness (gameplay gate, default OFF = stock): (S1) Mover::calcAttackChance floors the aimed-shot modifiers at Head 0.30 / Arm 0.45...", passthrough | registry(Feature, "MC2_FEATURE_CALLED_SHOT_FIX", -1, false) | verified),
L(Lead0Kill, "MC2_CAMERA_ASPECT_NATIVE", Bool, true, "code", "", "", passthrough | verified),
// history: docs/gate-history.md#MC2_CAMERA_PARK
G("MC2_CAMERA_PARK", String, "", "gameos", "", "CAMERA-PARK-1 (ported from vk branch 4b69ef78): ='x,y,rot,proj,alt[@frame]' hard-sets the live camera pose once at frame >= @frame (default 300) and pins it...", passthrough | registry(Trace, "MC2_DIAG_CAMERA_PARK", -1, false) | verified),
P("MC2_CAMERA_SMOOTH", Float, 100.0, "code", "", "", passthrough | clampRange(0.0, 2000.0) | verified),
L(Lead1, "MC2_CAMERA_SMOOTH_TRACE", Bool, false, "code", "", "", passthrough | verified),
L(Lead1, "MC2_CAM_SMOOTH_TEST", Bool, false, "code", "", "", passthrough | verified),
G("MC2_CAPTURE_FOCUS", String, "", "gameos", "", "Dev capture knob (read when a bookmark capture applies a pose): mech[:N] moves every bookmark pose onto the Nth live player-team mech (default 0); unset = off.", passthrough | verified),
P("MC2_CAPTURE_FOCUS_ZOOM", Float, 200.0, "gameos", "", "Camera altitude used with MC2_CAPTURE_FOCUS (default 200 frames a mech at about a fifth of the frame).", passthrough | clampRange(20.0, 6000.0) | verified),
G("MC2_CAPTURE_FORCE_BUILDING_HIGHLIGHT", String, "", "props", "", "CAPTURE-ONLY (parity row 28): hex ARGB forced as every building's highlight; read only while MC2_VISUAL_BOOKMARK_CAPTURE arms the replay.", passthrough | verified),
// history: docs/gate-history.md#MC2_CEMENT_ATLAS_DUMP
G("MC2_CEMENT_ATLAS_DUMP", String, "", "terrain", "", "CEMENT-ATLAS-CELL-2: dump the assembled cement/road overlay atlas so cell art can be inspected directly -- which cells actually carry painted markings is an...", passthrough | registry(Trace, "MC2_TRACE_CEMENT_ATLAS_DUMP", -1, false) | verified),
L(Lead0Kill, "MC2_CEMENT_ATLAS_SEAM_FIX", Bool, true, "terrain", "", "", verified),
// CEMENT-ATLAS-CELL-2: the two arms of the "concrete shouldn't magically
// become a different color" fix, scoped to ACTUAL concrete only.
// RUNWAY-APRON-TONE-2: cross-map absolute runway/apron tone normalisation.
L(Lead0Kill, "MC2_CEMENT_MISSION_LIGHT", Bool, true, "gameos", "", "", verified),
// MC2-03-CEMENT-PINK-1: cement catalog atlas post-processing (build-time, CPU).
// CEMENT-INTRACELL-1 (ARM C): make the concrete arms obey the user's own
// scope ruling by detecting the BAKED ROAD ART inside solid cement cells.
L(Presence, "MC2_CHEAT_INFINITE_MONEY", Bool, false, "mech", "", "Interactive cheat gates -- read once at startup, default OFF (unset = nullptr = false)", verified),
L(Presence, "MC2_CHEAT_SALVAGE_ALL", Bool, false, "mech", "", "", verified),
L(Lead0, "MC2_CLOUD_SHADOW", Bool, false, "post", "", "", passthrough | verified),
G("MC2_COALESCE_FORCE_DISARM", Enum, "", "props", "", "Step 12B.1 - exact, case-sensitive string match on the four valid values; anything else (including unset) leaves None.", tokens("|alloc_failed|mixed_alpha|no_extension|size_mismatch") | verified),
L(Lead0, "MC2_COLLISION_FACTS", Bool, false, "code", "", "", verified),
// history: docs/gate-history.md#MC2_COLORMAP_CPU_RETIRE
L(Lead0Kill, "MC2_COLORMAP_CPU_RETIRE", Bool, true, "terrain", "", "COLORMAP-CPU-RETIRE-1: free cpuColorMap + cpuDispAlpha in BuildColormapAtlas immediately after GPU atlas upload.", registry(Feature, "MC2_FEATURE_COLORMAP_CPU_RETIRE", 19, true) | verified),
// history: docs/gate-history.md#MC2_COLORMAP_DISPLACE_PROBE
L(Presence, "MC2_COLORMAP_DISPLACE_PROBE", Bool, false, "code", "", "COLORMAP-CPU-RETIRE-1 parity probe: logs displacement magnitude (maxDispMag in wu) from the CPU colormap displacement path.", registry(Trace, "MC2_TRACE_COLORMAP_DISPLACE_PROBE", -1, false) | verified),
// history: docs/gate-history.md#MC2_COLORMAP_KTX2
L(Lead0Kill, "MC2_COLORMAP_KTX2", Bool, true, "terrain", "", "COLORMAP-BC7-KTX2-1: prefer .burnin.ktx2 BC7 sidecar in BuildColormapAtlas (gos_terrain_indirect.cpp).", registry(Feature, "MC2_FEATURE_COLORMAP_KTX2", 20, true) | verified),
G("MC2_COMMAND_REPLAY_PLAY", String, "", "code", "", "Play a command replay through the existing Replay queue source.", passthrough | registry(Trace, "MC2_COMMAND_REPLAY_PLAY", -1, false) | verified),
G("MC2_COMMAND_REPLAY_RECORD", String, "", "code", "", "Record accepted tactical commands and per-tick subsystem hashes.", passthrough | registry(Trace, "MC2_COMMAND_REPLAY_RECORD", -1, false) | verified),
P("MC2_COMMAND_REPLAY_TICKS", Int, 180, "gameos", "", "Number of fixed ticks in a command replay recording.", range(1, 108000) | passthrough | verified),
L(Presence, "MC2_CONE_LEAF_DUMP", Bool, false, "props", "", "CONE-LEAF-NAME-DIAG (batch-8C, MC2_CONE_LEAF_DUMP, default OFF): dump every static-prop recipe leaf's real node name + flags.", verified),
// history: docs/gate-history.md#MC2_CONE_POOL_POLICY
L(Lead0Kill, "MC2_CONE_POOL_POLICY", Bool, true, "code", "", "CONE-SPILL-POLICY-1: pool-placement policy for building/beacon spotlight ground pools on the DEFAULT path (MC2_CONE_SPILL_COUPLING OFF).", passthrough | registry(Feature, "MC2_FEATURE_CONE_POOL_POLICY", -1, true) | verified),
// history: docs/gate-history.md#MC2_CONE_POOL_REACH
P("MC2_CONE_POOL_REACH", Float, 4.0, "code", "", "CONE-SPILL-POLICY-1: reach multiplier k for the extended beam/terrain intersection search (reach = k * authored coneLen * MC2_LIGHT_CONE_SPILL).", passthrough | clampRange(1.0, 16.0) | registry(Trace, "MC2_TUNE_CONE_POOL_REACH", -1, false) | verified),
// history: docs/gate-history.md#MC2_CONE_SPILL_COUPLING
L(Lead1, "MC2_CONE_SPILL_COUPLING", Bool, false, "code", "", "CONE-SPILL-GRADE-1: isolates CONE-SPILL-COUPLING-1 (c1f98b45), which shipped UNGATED as a default-behaviour change riding MC2_LIGHT_CONE_NODES and is now...", passthrough | registry(Trace, "MC2_CONE_SPILL_COUPLING", -1, false) | verified),
G("MC2_CONFIG_DUMP", Enum, "default", "gameos", "", "[CONFIG v1] dump scope: default (env-set gates), all (every row), none (summary only)", tokens("default|all|none")),
G("MC2_CONFIG_STRICT", Bool, false, "gameos", "", "make a bad value or an unregistered MC2_* variable fatal at startup"),
G("MC2_CONTRACT_REPO_ROOT", String, "", "unclassified", "", ""),
// ── CPU-PHONG-RETIRE-1 ─────────────────────────────────────────────────
// Lives in kAuxEnvVars, not kFeatureTable: kFeatureTable is indexed by
// RendererFeature, and minting an enum ordinal for a gate that guards a
// CPU gameplay-elevation term (no render path) buys nothing. Same placement
// rationale as MC2_ASSET_FAILLOUD above, which is also a default-ON
// killswitch registered here.
L(Lead1, "MC2_CPU_PROJ_COST_SPLIT", Bool, false, "code", "", "", passthrough | verified),
L(Presence, "MC2_CRASH_FAST_EXIT", Bool, false, "gameos", "", "Headless crash fast-exit: write the bundle, then terminate WITHOUT the modal dialog so an automated soak/harness process exits immediately on crash (the...", verified),
L(Presence, "MC2_CURSOR_TARGET_TRACE", Bool, false, "code", "", "", verified),
G("MC2_DEBUG_OVERLAY_PREDICATE_MODE", Enum, "", "code", "", "", tokens("|Legacy|Modern") | verified),
L(Lead0, "MC2_DEBUG_RENDERER_TEST", Bool, false, "gameos", "", "", verified),
// history: docs/gate-history.md#MC2_DEBUG_STATE_DUMP
L(Lead0, "MC2_DEBUG_STATE_DUMP", Bool, false, "gameos", "", "DEBUG-STATE-DUMP-1: write a JSON render-state snapshot to debug_state/latest_render_state.json every 300 frames (and at frame 1).", passthrough | registry(Trace, "MC2_DIAG_DEBUG_STATE_DUMP", -1, false) | verified),
// history: docs/gate-history.md#MC2_DEBUG_STATE_DUMP_DIR
G("MC2_DEBUG_STATE_DUMP_DIR", String, "", "gameos", "", "DEBUG-STATE-DUMP-1: override output directory for MC2_DEBUG_STATE_DUMP.", passthrough | registry(Trace, "MC2_DIAG_DEBUG_STATE_DUMP_DIR", -1, false) | verified),
// history: docs/gate-history.md#MC2_DEBUG_STATE_DUMP_HISTORY
L(Lead0, "MC2_DEBUG_STATE_DUMP_HISTORY", Bool, false, "gameos", "", "DEBUG-STATE-DUMP-2: enable rolling 8-slot history ring alongside latest_render_state.json.", passthrough | registry(Trace, "MC2_DIAG_DEBUG_STATE_DUMP_HISTORY", -1, false) | verified),
L(Presence, "MC2_DEBUG_VIDEO", Bool, false, "code", "", "", verified),
// history: docs/gate-history.md#MC2_DEBUG_VIEW
P("MC2_DEBUG_VIEW", Int, 0, "code", "", "DEBUG-VIEW-SWEEP-1: =N (1..6) boot-seeds the global full-scene debug render view: 1=shadow-factor 2=normalized-depth 3=unlit-albedo 4=overdraw-proxy...", registry(Trace, "MC2_DIAG_DEBUG_VIEW", -1, false) | verified),
// history: docs/gate-history.md#MC2_DEBUG_VIEW_SWEEP
G("MC2_DEBUG_VIEW_SWEEP", String, "", "code", "", "DEBUG-VIEW-SWEEP-1: ='<startFrame>,<settleFrames>' frame-scheduled auto-sweep of the six MC2_DEBUG_VIEW views (missiongui.cpp update).", registry(Trace, "MC2_DIAG_DEBUG_VIEW_SWEEP", -1, false) | verified),
L(Presence, "MC2_DECOR_SHADOW_TRACE", Bool, false, "code", "", "", verified),
G("MC2_DEFS_ROOT", String, "", "unclassified", "", "", verified),
L(Presence, "MC2_DEPTHBIAS_CALIB", Bool, false, "terrain", "", "", verified),
L(Presence, "MC2_DEPTH_TRANSITION_PROBE", Bool, false, "terrain", "", "", passthrough | verified),
L(Presence, "MC2_DESTROY_TRACE", Bool, false, "gameos", "", "", passthrough | verified),
// DEV-DETERMINISM: single shared RNG stream for reproducible runs.
// history: docs/gate-history.md#MC2_DETERMINISTIC_RNG
L(Lead0Kill, "MC2_DETERMINISTIC_RNG", Bool, true, "mech", "", "Route all randomness (incl.", passthrough | registry(Feature, "MC2_DETERMINISTIC_RNG", -1, true) | verified),
L(Lead1, "MC2_DEV_SHELL", Bool, false, "unclassified", "", "", verified),
P("MC2_DEV_SHELL_PORT", Int, 0, "gameos", "", "", verified),  // default not derivable from the tree
G("MC2_DIAGNOSTIC_TRACE_FILE", String, "", "unclassified", "", "", passthrough | verified),  // default not derivable from the tree
G("MC2_DIAG_TAGS", String, "", "gameos", "", "", passthrough | verified),
// history: docs/gate-history.md#MC2_DIAG_VERBOSE
L(Lead0, "MC2_DIAG_VERBOSE", Bool, false, "gameos", "", "QUIET-DEFAULT-BUILD-1: ONE umbrella gate for the engine's undifferentiated developer chatter.", passthrough | registry(Trace, "MC2_TRACE_DIAG_VERBOSE", -1, false) | verified),
// history: docs/gate-history.md#MC2_DIRECT_FIRE_STRAIGHT
L(Exact1, "MC2_DIRECT_FIRE_STRAIGHT", Bool, false, "code", "", "Direct-fire de-curve: PPC/autocannon/gauss bolts fly STRAIGHT instead of re-tracking the moving target every frame (the janky homing curve).", passthrough | registry(Feature, "MC2_DIRECT_FIRE_STRAIGHT", -1, false) | verified),
L(Lead1, "MC2_DISABLE_GOSFX", Bool, false, "code", "", "", passthrough | verified),
L(Lead1, "MC2_DPI_UNAWARE", Bool, false, "gameos", "", "", verified),
L(Lead1, "MC2_DRAW_PACKET_COMPARE", Bool, false, "gameos", "", "", passthrough | verified),
L(Lead1, "MC2_DRAW_PACKET_COMPARE_VERBOSE", Bool, false, "gameos", "", "", passthrough | verified),
L(Lead1, "MC2_DRAW_PACKET_V3", Bool, false, "gameos", "", "", passthrough | verified),
L(Lead1, "MC2_DRAW_PACKET_WARN", Bool, false, "gameos", "", "", verified),
L(Lead0Kill, "MC2_EDGE_FOG", Bool, true, "post", "", "", verified),
P("MC2_EDGE_FOG_HEIGHT", Float, 0.0, "post", "", "", verified),  // default not derivable from the tree
P("MC2_EDGE_FOG_MAX", Float, 0.0, "post", "", "", verified),  // default not derivable from the tree
P("MC2_EDGE_FOG_START", Float, 0.0, "post", "", "", verified),  // default not derivable from the tree
L(Lead0Kill, "MC2_EDITOR_AUTODOCK", Bool, true, "code", "", "", verified),
L(Lead0, "MC2_EDITOR_BEAUTY_AUTOAPPLY", Bool, false, "editor", "", "", verified),
L(Lead0Kill, "MC2_EDITOR_DOCK", Bool, true, "code", "", "", verified),
P("MC2_EDITOR_FAR_CLIP", Float, 0.0, "code", "", "", verified),  // default not derivable from the tree
P("MC2_EDITOR_FPS_CAP", Int, 60, "editor", "", "Editor frame-rate cap; <= 0 is uncapped.", passthrough | verified),
L(Lead0Kill, "MC2_EDITOR_GPU_PICK", Bool, true, "editor", "", "", verified),
L(Exact1, "MC2_EDITOR_GPU_TIMERS", Bool, false, "editor", "", "", verified),
L(Exact1, "MC2_EDITOR_MODE", Bool, false, "editor", "", "", verified),
L(Lead0Kill, "MC2_EDITOR_MOVER_REFRESH", Bool, true, "editor", "", "", verified),
P("MC2_EDITOR_PANEL_W", Int, 320, "editor", "", ""),
L(Presence, "MC2_EDITOR_PICK_TRACE", Bool, false, "editor", "", "", verified),
L(Presence, "MC2_EDITOR_STATIC_PREWARM_OFF", Bool, false, "editor", "", "", verified),
L(Presence, "MC2_EDITOR_STATIC_PRIME_DIAG", Bool, false, "props", "", "MC2_EDITOR_STATIC_PRIME_DIAG=1 - compact aggregate DrawCmd census at finalize.", verified),
L(Presence, "MC2_EDITOR_TRACE", Bool, false, "editor", "", "", verified),
G("MC2_EDITOR_UI_DEFS", Bool, true, "editor", "", "Mission Editor shell overlay is data/defs-first by default.", verified),
L(Exact1, "MC2_EDITOR_WATCHDOG", Bool, false, "editor", "", "", verified),
G("MC2_EFFECT_ADMISSION_PREDICATE", Enum, "", "code", "", "", tokens("|legacy|modern") | verified),
L(Presence, "MC2_ENGINE_XL", Bool, false, "mech", "", "", verified),
L(Lead0, "MC2_EQUIPMENT_ADD", Bool, false, "code", "", "", verified),
G("MC2_EQUIPMENT_DEFINITION_SOURCE", Enum, "legacy", "code", "", "", passthrough | tokens("legacy|generated") | verified),
// FarBackdropLit (FAR-BACKDROP-LIT-IMPL-1)
// history: docs/gate-history.md#MC2_FAR_BACKDROP_LIT
// history: docs/gate-history.md#MC2_FAR_BACKDROP_LIT_FAR
// history: docs/gate-history.md#MC2_FAR_BACKDROP_LIT_NEAR
// history: docs/gate-history.md#MC2_FAR_BACKDROP_LIT_STRENGTH
// history: docs/gate-history.md#MC2_FAR_BACKDROP_LIT_TINT
L(Lead0P, "MC2_FASTPATH_DROP_LOG", Bool, false, "terrain", "", "", passthrough | verified),
L(Presence, "MC2_FF_TRACE", Bool, false, "unclassified", "", "", verified),
// history: docs/gate-history.md#MC2_FIRE_DECISION_TRACE
L(Lead0P, "MC2_FIRE_DECISION_TRACE", Bool, false, "mech", "", "ISSUE-17 fire-refusal instrumentation (default OFF = byte-identical; needs MC2_DIAG_TAGS=...,COMBAT).", passthrough | registry(Trace, "MC2_TRACE_FIRE_DECISION", -1, false) | verified),
// history: docs/gate-history.md#MC2_FIXTURE_ORDER_LIVENESS
L(Lead0P, "MC2_FIXTURE_ORDER_LIVENESS", Bool, false, "code", "", "META-FIX design F behavioral fixture (code/mission.cpp soak namespace; requires MC2_SOAK_AUTOWIN): at 20% of the autowin timer every armed home mech gets a...", passthrough | registry(Trace, "MC2_TRACE_FIXTURE_ORDER_LIVENESS", -1, false) | verified),
P("MC2_FIXTURE_ORDER_MOVE_M", Float, 300.0, "code", "", "Meters the MC2_FIXTURE_ORDER_LIVENESS move order travels toward the map centre (default 300); long values drive a cross-map path on big maps.", clampRange(1.0, 1.0e7) | verified),
// FOG-HORIZON-CLAMP-1: reshape OOB/edge fog to a horizon elevation profile.
L(Lead0, "MC2_FORCE_43", Bool, false, "gameos", "", "Default-OFF gate (matches the launcher toggle model: checked -> MC2_FORCE_43=1, unchecked -> unset).", passthrough | verified),
// history: docs/gate-history.md#MC2_FORCE_ALL_VISIBLE
L(Lead0, "MC2_FORCE_ALL_VISIBLE", Bool, false, "code", "", "FORCE-ALL-VISIBLE-1: mark every unit (mechs, ground vehicles, elementals, all teams) fully visible and sensed, bypassing sensor / contact gating so nothing...", passthrough | registry(Trace, "MC2_TRACE_FORCE_ALL_VISIBLE", -1, false) | verified),
L(Lead0P, "MC2_FORCE_DYNAMIC_BUILDINGS", Bool, false, "props", "", "", verified),  // default not derivable from the tree
L(Lead0P, "MC2_FORCE_DYNAMIC_TREES", Bool, false, "props", "", ""),  // default not derivable from the tree
