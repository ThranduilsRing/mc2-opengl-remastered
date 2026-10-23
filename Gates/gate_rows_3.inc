L(Lead1, "MC2_SHADER_HOT_RELOAD", Bool, false, "gameos", "", "", verified),
P("MC2_SHADER_PATH_TINT", Int, 0, "post", "", "", verified),
// SPIRV-CONSUMER-PILOT-BUILD-1: runtime SPIR-V consumer (postprocess pilot).
// history: docs/gate-history.md#MC2_SHADER_SPIRV
// history: docs/gate-history.md#MC2_SHADER_SPIRV_FATAL
// history: docs/gate-history.md#MC2_SHADOW_BOUNDED_NEAR_RADIUS
// history: docs/gate-history.md#MC2_SHADOW_CASTER_CULL_CACHE
// history: docs/gate-history.md#MC2_SHADOW_DEBUG_MODE
G("MC2_SHADOW_DEBUG_MODE", String, "", "post", "", "SHADOW-ENV-DEBUG-MODE-1: selects shadow debug map overlay without ImGui.", passthrough | registry(Trace, "MC2_DIAG_SHADOW_DEBUG_MODE", -1, false) | verified),
L(Presence, "MC2_SHADOW_DIAG", Bool, false, "props", "", "", verified),
// history: docs/gate-history.md#MC2_SHADOW_DYNAMIC_PROP_CASTERS
// history: docs/gate-history.md#MC2_SHADOW_ENABLE
L(Lead0Kill, "MC2_SHADOW_ENABLE", Bool, true, "unclassified", "", "Dynamic object shadow caster pass (GpuStaticPropBatcher + GpuMechBatcher flushShadow).", passthrough | registry(Feature, "MC2_FEATURE_SHADOW_MAPS", 5, true) | verified),
// history: docs/gate-history.md#MC2_SHADOW_FRUSTUM_DIAG
// history: docs/gate-history.md#MC2_SHADOW_GUARD_CLAMPED_SUN
P("MC2_SHADOW_MECH_SOFT", Float, 1.0, "unclassified", "", "", clampRange(0.0, 4.0) | verified),
// history: docs/gate-history.md#MC2_SHADOW_MIN_SUN_ALT
P("MC2_SHADOW_MIN_SUN_ALT", Float, 15.0, "post", "", "SHADOW-SUN-ALT-CLAMP-1 tunable: minimum EFFECTIVE sun altitude in DEGREES used by the shadow projection matrices (mc2ClampShadowSunAltitude...", passthrough | registry(Trace, "MC2_TUNE_SHADOW_MIN_SUN_ALT", -1, false) | verified),
L(Lead0Kill, "MC2_SHADOW_PROP_ALPHA", Bool, true, "props", "", "", passthrough),
L(Lead0Kill, "MC2_SHADOW_PROP_ALPHA_ATLAS", Bool, true, "props", "", "", passthrough),
L(Lead0Kill, "MC2_SHADOW_ROBUST_BASIS", Bool, true, "post", "", ""),
L(Presence, "MC2_SHADOW_TIER_STATS", Bool, false, "terrain", "", "", verified),
L(Presence, "MC2_SHAPE_C_PARITY_CHECK", Bool, false, "terrain", "", "", passthrough | verified),
// ShorelineLegacySuppress (SHORELINE-GLOW-1)
// kFeatureTable is indexed by RendererFeature, so this entry's position is
// its enum value (58).
L(Presence, "MC2_SHOT_RESULTS", Bool, false, "code", "", "", verified),
L(Presence, "MC2_SIM_CLOCK_TRACE", Bool, false, "code", "", "", passthrough | verified),
// ── SIM-IDENTITY-HASH-1 ────────────────────────────────────────────────
// Per-sim-frame world-state hash (mover positions / orientations / gesture
// + anim phase / target ids) feeding the refuse-to-grade gate in
// scripts/sim_identity.py. Mover nondeterminism is a DESYNC, not a metrics
// problem; this is the lockstep-game instrument for it.
//
// `defaultOn = true` is deliberate and load-bearing, NOT a copy-paste slip.
// The consumer is a gate that REFUSES to compare two capture arms whose
// worlds differ. A default-OFF producer would leave the block absent from
// every manifest, and "absent" is the one input the gate cannot grade -- so
// the refusal would fire on everything on day one, get called noise, and be
// switched off. Classified Trace because it emits diagnostic output only:
// the collector is read-only (getters only; the mutating zero-arg
// getMovePath() is deliberately never called) and changes no render, no
// gameplay and no sim state.
//
// The DEFAULT IS READ FROM code/sim_identity.cpp envOnOff(), which parses
// "0"/"false"/"off"/"no" as OFF -- unlike the neighbouring
// MC2_BRIDGE_MOVER_STATE, which is `getenv() != nullptr` and therefore
// ENABLES on `=0`. Do not "harmonize" these two; the difference is the fix.
// history: docs/gate-history.md#MC2_SIM_IDENTITY
G("MC2_SIM_IDENTITY", Bool, true, "code", "", "SIM-IDENTITY-HASH-1: emit a world-state hash (sorted-by-partId digest over mover position/rotation/gesture/anim-frame/damage/status/team plus...", passthrough | registry(Trace, "MC2_SIM_IDENTITY", -1, true) | verified),
// history: docs/gate-history.md#MC2_SIM_IDENTITY_TRACE
G("MC2_SIM_IDENTITY_TRACE", Bool, false, "code", "", "SIM-IDENTITY-HASH-1: emit one `[SIMID v1] simframe=N t=T n=C graded=.. visual=.. strict=..` line per SIM frame to stdout.", passthrough | registry(Trace, "MC2_SIM_IDENTITY_TRACE", -1, false) | verified),
// history: docs/gate-history.md#MC2_SIM_IDENTITY_TRACE_DETAIL
G("MC2_SIM_IDENTITY_TRACE_DETAIL", Bool, false, "code", "", "SIM-IDENTITY-HASH-1: with MC2_SIM_IDENTITY_TRACE on, additionally emit one `[SIMID v1 mover]` line per mover per sim frame.", passthrough | registry(Trace, "MC2_SIM_IDENTITY_TRACE_DETAIL", -1, false) | verified),
P("MC2_SIM_SPEED", Float, 1.0, "code", "", "SIM-SPEED-BUTTON-1: play (wall clock) = seed of the HUD/dev-shell dt multiplier; harness fixed-step clock, unfocused = floor(N) 1/30 s steps per drawn frame.", passthrough | clampRange(0.25, 4.0) | verified),
L(Lead0, "MC2_SKIP_STATIC_BUILDINGS", Bool, false, "code", "", "", verified),
L(Lead0Kill, "MC2_SKIP_STATIC_TREES", Bool, true, "code", "", "", verified),
L(Presence, "MC2_SKIP_STATIC_TREES_DIAG", Bool, false, "code", "", "", verified),
// BACKEND-DEFAULT-PARITY-1: skybox fog exclusion (was unregistered, and that
// is what let two readers disagree in silence for the whole life of the gate).
// history: docs/gate-history.md#MC2_SMART_LOAD
L(Lead0Kill, "MC2_SMART_LOAD", Bool, true, "code", "", "SMART-LOAD startup/mission-load perf path (logisticsdata.cpp).", registry(Feature, "MC2_FEATURE_SMART_LOAD", -1, true) | verified),
L(Presence, "MC2_SMART_LOAD_TRACE", Bool, false, "code", "", "", verified),
L(Lead0, "MC2_SMOKE_ACTIVE", Bool, false, "editor", "", "Editor smoke forwards --smoke-active to the playtest child.", passthrough | verified),
L(Presence, "MC2_SMOKE_FIXED_TIMESTEP", Bool, false, "gameos", "", "S9D deterministic clock.", passthrough | verified),
L(Lead0, "MC2_SMOKE_FREEZE_ANIM", Bool, false, "mclib", "", "Freeze mech gait and building animation frames (capture and SIMID determinism).", passthrough | verified),
L(Presence, "MC2_SMOKE_MODE", Bool, false, "gameos", "", "", passthrough | verified),
P("MC2_SMOKE_PERF_SAMPLES", Int, 0, "gameos", "", "", verified),  // default not derivable from the tree
G("MC2_SMOKE_SEED", String, "", "gameos", "", "", passthrough | verified),  // default not derivable from the tree
// history: docs/gate-history.md#MC2_SNAPSHOT_MECH_EXTRACT
L(Lead0, "MC2_SNAPSHOT_MECH_EXTRACT", Bool, false, "mech", "", "MECH-EXTRACTION-0: mech snapshot extraction (observe-only, no GL mutation).", passthrough | registry(Feature, "MC2_FEATURE_SNAPSHOT_MECH_EXTRACT", 13, false) | verified),
L(Presence, "MC2_SOAK_AUTOWIN", Bool, false, "code", "", "", verified),
L(Presence, "MC2_SOAK_AUTO_PURCHASE", Bool, false, "code", "", "SOAK-AUTO-PURCHASE-1 (MC2_SOAK_AUTO_PURCHASE).", verified),
L(Presence, "MC2_SOAK_CHECK_SCREENS", Bool, false, "code", "", "SOAK-SCREEN-CHECK-1 (MC2_SOAK_CHECK_SCREENS).", verified),
L(Presence, "MC2_SOAK_KILL_ENEMY", Bool, false, "code", "", "--- MC2_SOAK_KILL_ENEMY ------------------------------------------------- When set, kills one random live enemy BattleMech before autowin fires so the...", verified),
L(Presence, "MC2_SOAK_LANCE_RANDOM", Bool, false, "code", "", "SOAK-LANCE-RANDOM-1 (MC2_SOAK_LANCE_RANDOM).", verified),
L(Presence, "MC2_SOAK_PILOT_PROMOTE", Bool, false, "code", "", "--- MC2_SOAK_PILOT_PROMOTE ---------------------------------------------- Grants a deployed pilot enough skill delta to cross the next rank threshold, then...", verified),
// GitHub-issue fix batch 2026-07-16 (issues #42a, #50, #34)
// history: docs/gate-history.md#MC2_SOAK_SALVAGE_TRADEMOVER
L(Presence, "MC2_SOAK_SALVAGE_TRADEMOVER", Bool, false, "code", "", "ISSUE-42a repro fixture (code/mission.cpp soak harness): with MC2_SOAK_AUTOWIN + MC2_SOAK_KILL_ENEMY, exercises the REAL mid-mission salvage-craft flow...", passthrough | registry(Trace, "MC2_TRACE_SOAK_SALVAGE_TRADEMOVER", -1, false) | verified),
L(Presence, "MC2_SOAK_STOP_AT_BAY", Bool, false, "code", "", "SOAK-STOP-AT-BAY-1 (MC2_SOAK_STOP_AT_BAY).", verified),
P("MC2_SOAK_WIN_AFTER_SEC", Float, 5.0, "code", "", "Seconds of scenario time before the soak autowin fires; negative clamps to 0.", clampRange(0.0, 3.4e38) | verified),
// META-FIX-BATCH-1 (recurrence-proofing, designs C + E — 2026-07-16)
// history: docs/gate-history.md#MC2_SPOTLIGHT_DIRECTION_FIX
G("MC2_SPOTLIGHT_DIRECTION_FIX", Enum, "0", "code", "", "SPOTLIGHT-DIRECTION-1: fixes two coordinate-space defects at the building/guard-tower spotlight registration site (mclib/bdactor.cpp).", passthrough | registry(Trace, "MC2_SPOTLIGHT_DIRECTION_FIX", -1, false) | tokens("0|1|2") | verified),
L(Presence, "MC2_SPOTLIGHT_REAL_TRACE", Bool, false, "unclassified", "", "[SPOTLIGHT_REAL_TRACE v1] (E) Stage 0 / T0.2 baseline.", verified),
L(Presence, "MC2_SPOT_DIAG", Bool, false, "mech", "", "T1.15 SpotLight_ illumination diagnostic - registration probe (bldg class).", verified),
// SP-BATCHER-ALPHASCAN-GATE-1 (framebudget-2 perf).
// BUILDING-GLASS-1: dither-glass translucency for static-prop ALPHA_TEST packets.
// SSAO-BLUR-1: the missing denoise. Killswitch, NOT an opt-in -- default ON
// whenever SSAO runs, because the un-blurred chain is not a shippable config.
G("MC2_STABLE_LIGHT_SKIP", Bool, false, "props", "", "Std Bool row: any text other than 0/false/off/no/1/true/on/yes (e.g. =2) still reads as on but is reported badvalue, which MC2_CONFIG_STRICT=1 turns fatal.", passthrough | verified),
G("MC2_STABLE_LIGHT_SKIP_DIAG", Bool, false, "props", "", "Std Bool row: any text other than 0/false/off/no/1/true/on/yes (e.g. =2) still reads as on but is reported badvalue, which MC2_CONFIG_STRICT=1 turns fatal.", verified | passthrough),
G("MC2_STABLE_LIGHT_SKIP_TOUCH", Bool, false, "props", "", "Std Bool row: any text other than 0/false/off/no/1/true/on/yes (e.g. =2) still reads as on but is reported badvalue, which MC2_CONFIG_STRICT=1 turns fatal.", verified | passthrough),
// history: docs/gate-history.md#MC2_STATICPROP_BC7
// Static-prop ORM (PBR) — load-only feature + trace
// history: docs/gate-history.md#MC2_STATICPROP_MATERIAL_PBR_SLOTS
L(Lead0Kill, "MC2_STATICPROP_RECIPE_SPOTLIGHT_SKIP", Bool, true, "props", "", "", passthrough | verified),
P("MC2_STATICPROP_TEX_TIER", Int, 512, "props", "", "", verified),
L(Presence, "MC2_STATIC_FORCE_ADMIT", Bool, false, "props", "", "2026-05-10 diag: MC2_STATIC_FORCE_ADMIT mirrors registry::flush behavior so the env-var A/B (cull-bypassed vs cull-active) covers BOTH substrate-record...", passthrough | verified),
L(Presence, "MC2_STATIC_LEAF_DIAG", Bool, false, "props", "", "", verified),
// history: docs/gate-history.md#MC2_STATIC_LIGHT_UPLOAD_SPLIT
L(Lead0Kill, "MC2_STATIC_LIGHT_UPLOAD_SPLIT", Bool, true, "code", "", "STATICPROP-PERMANENT-INSTANCE-LIGHTS-1: upload the immutable static light prefix [0..S) once/dirty-only + the dynamic suffix [S..count) per frame, instead...", registry(Feature, "MC2_FEATURE_STATIC_LIGHT_UPLOAD_SPLIT", -1, true)),
G("MC2_STATIC_PER_INSTANCE_LIGHT", Bool, true, "props", "", "2026-05-11 per-instance light-idx capture.", verified),
L(Presence, "MC2_STATIC_POP_SPLIT", Bool, false, "props", "", "", verified),
L(Presence, "MC2_STATIC_PROP_BAKE_SELFTEST", Bool, false, "gameos", "", "", verified),
// history: docs/gate-history.md#MC2_STATIC_PROP_BUILDING_SHADOW_REBAKE_TEST_DESTROY_AT_FRAME
P("MC2_STATIC_PROP_BUILDING_SHADOW_REBAKE_TEST_DESTROY_AT_FRAME", Int, -1, "code", "", "STATIC-BLDG-SHADOW-REBAKE-1: deterministic repro hook (test-only, no production callers).", passthrough | registry(Trace, "MC2_STATIC_PROP_BUILDING_SHADOW_REBAKE_TEST_DESTROY_AT_FRAME", -1, false) | verified),
// SPFLUSH-COST-SPLIT-1: RDTSC decomposition of StaticPropRegistryFlush hot zone.
P("MC2_STATIC_PROP_GLOBAL_CAP", Int, 131072, "props", "", "", verified),
L(Lead0P, "MC2_STATIC_PROP_GLOBAL_POOL_LEGACY", Bool, false, "props", "", "", verified),
// ── IBL-ROTATION-LAND-1 ────────────────────────────────────────────────
// Both rows live in kAuxEnvVars, not kFeatureTable: kFeatureTable is
// INDEXED BY RendererFeature and neither of these needs an enum ordinal --
// nothing dispatches on them, the only consumer is a uniform upload. Same
// placement rationale as MC2_FEATURE_WATER_SHORE_MEANDER above.
G("MC2_STATIC_PROP_LATE_SPAWN_REG", Bool, false, "props", "", "Set MC2_STATIC_PROP_LATE_SPAWN_REG=1 to opt in (default OFF as of 2026-05-20).", verified),
G("MC2_STATIC_PROP_MISSION_LOAD_REG", Bool, true, "props", "", "Task 9 (Track B): mission-load + late-spawn registration ON by default.", verified),
L(Lead0, "MC2_STATIC_PROP_PICK", Bool, false, "code", "", "", verified),
L(Lead0, "MC2_STATIC_PROP_PICK_DEBUG", Bool, false, "code", "", "", verified),
L(Lead0Kill, "MC2_STATIC_PROP_SNAPSHOT_FILL_DIRTYONLY", Bool, true, "gameos", "", "", passthrough | verified),
L(Lead0, "MC2_STATIC_PROP_TRACE", Bool, false, "props", "", "Stage 2.C+ instrumentation: one-shot registered-types dump.", verified),
L(Lead0Kill, "MC2_STATIC_RECIPE_RECYCLE", Bool, true, "props", "", "", verified),
L(Presence, "MC2_STATIC_REG_PREWARM", Bool, false, "code", "", "", passthrough | verified),
L(Presence, "MC2_STATIC_REG_PREWARM_TRACE", Bool, false, "code", "", "", passthrough | verified),
L(Presence, "MC2_STATIC_STALE_DROP_FATAL", Bool, false, "props", "", "Opt-in teeth for CI / bisection: abort on the regression signature.", passthrough | verified),
G("MC2_STATIC_UPDATE_TRACE", Bool, false, "terrain", "", "", verified),
L(Presence, "MC2_STRINGS_TRACE", Bool, false, "gameos", "", "Trace the string-table loader; any set value enables (even 0).", passthrough | verified),
L(Presence, "MC2_SUBMIT_TYPEHIST", Bool, false, "props", "", "", verified),
L(Presence, "MC2_SUBSTRATE_COALESCE_LEGACY", Bool, false, "props", "", "", verified),
L(Presence, "MC2_SUBSTRATE_COALESCE_TRACE", Bool, false, "props", "", "Step 12.1 - trace gate for noisy non-lifecycle COALESCE diagnostics.", verified),
// SunAxisFix (SUN-AXIS-FIX-1)
// kFeatureTable is indexed by RendererFeature, so this entry's position is
// its enum value (59).
// SunElevationFix (SUN-ELEVATION-FIX-1)
// kFeatureTable is indexed by RendererFeature, so this entry's position is
// its enum value (60).
L(Lead1, "MC2_TACMAP_FORMATION_LINE", Bool, false, "code", "", "", verified),
// history: docs/gate-history.md#MC2_TACTICAL_ARC_OVERLAY
L(Lead0, "MC2_TACTICAL_ARC_OVERLAY", Bool, false, "code", "", "TACTICAL-ARC-OVERLAY-MVP-1: world-space range ring + facing line on each SELECTED mover, drawn in-scene by the debug renderer just before its flush...", registry(Feature, "MC2_FEATURE_TACTICAL_ARC_OVERLAY", -1, false) | verified),
L(Lead0Kill, "MC2_TACTICAL_OVERVIEW", Bool, true, "code", "", "", verified),
L(Presence, "MC2_TACTICAL_OVERVIEW_DEBUG", Bool, false, "code", "", "Tactical Overview diagnostic HUD - only under MC2_TACTICAL_OVERVIEW_DEBUG.", verified),
L(Lead0Kill, "MC2_TACTICAL_OVERVIEW_TINT", Bool, true, "code", "", "", verified),
L(AtoiNZ, "MC2_TACTIC_WEIGHTS", Bool, false, "code", "", "TACTIC-WEIGHTS-A: load [Tactics] block from <missionName>_ai.fit into tacticWeights[].", verified),
L(AtoiNZ, "MC2_TACTIC_WEIGHTS_B", Bool, false, "code", "", "", verified),
L(Presence, "MC2_TARGETING_GUARD", Bool, false, "code", "", "", passthrough | verified),
L(Presence, "MC2_TERRAIN_ADMISSION_LEGACY", Bool, false, "terrain", "", "", verified),
// ── TERRAIN-AMBIENT-FLOOR-2 ────────────────────────────────────────────
// DEFAULT-ON visual change. `defaultOn` below is `true` ON PURPOSE and must
// stay true: the predecessor slice registered the identical gate as
// defaultOn=false while its C++ default was an ACTIVE 0.35, so every gate
// audit, every dead-gate scan and every release note would have reported a
// shipped visual change as OFF. Classified Feature, not Trace, for the same
// reason -- Trace means "diagnostic output only; no correctness effect",
// which this is not.
// history: docs/gate-history.md#MC2_TERRAIN_AMBIENT_FLOOR
P("MC2_TERRAIN_AMBIENT_FLOOR", Float, 0.35, "terrain", "", "TERRAIN-AMBIENT-FLOOR-2: terrain-only, albedo-scaled ambient floor in terrain_lod_chunk.frag.", passthrough | registry(Feature, "MC2_FEATURE_TERRAIN_AMBIENT_FLOOR", -1, true) | clampRange(0.0, 2.0) | verified),
// ── TERRAIN-AMBIENT-FLOOR-NIGHT-1 ──────────────────────────────────────
// DEFAULT-OFF companion to the row above. Registered as a Feature (not a
// Trace) because it changes pixels when armed. kAuxEnvVars is not indexed by
// RendererFeature, so this row's position is free.
// history: docs/gate-history.md#MC2_TERRAIN_AMBIENT_FLOOR_NIGHT
P("MC2_TERRAIN_AMBIENT_FLOOR_NIGHT", Float, 0.0, "terrain", "", "TERRAIN-AMBIENT-FLOOR-NIGHT-1: 0..1 FLOOR under TERRAIN-AMBIENT-FLOOR-2's daylight gate; the uploaded gate becomes max(dayGate, floor).", passthrough | registry(Feature, "MC2_FEATURE_TERRAIN_AMBIENT_FLOOR_NIGHT", -1, false) | clampRange(0.0, 1.0) | verified),
// history: docs/gate-history.md#MC2_TERRAIN_AMBIENT_FLOOR_TRACE
// history: docs/gate-history.md#MC2_TERRAIN_CLIFF_DISPLACE
// history: docs/gate-history.md#MC2_TERRAIN_CLIFF_DISPLACE_MAXTESS
P("MC2_TERRAIN_CLIFF_DISPLACE_MAXTESS", Float, 4.0, "terrain", "", "SLICE 3 knob: max tessellation factor (float; C++ default 4.0, clamped to GL_MAX_TESS_GEN_LEVEL at program build).", passthrough | registry(Trace, "MC2_TUNE_TERRAIN_CLIFF_MAXTESS", -1, false)),
L(Lead0, "MC2_TERRAIN_CLIFF_SHADOW_FLOOR", Bool, false, "terrain", "", "", verified),
// TERRAIN-CLIFF-MATERIAL-TRIPLANAR-1: world-axis triplanar ROCK NORMAL + relief
// on steep terrain cells (terrain_lod_chunk.frag useTriplanarCliff branch,
// gos_terrain_lod_chunk.cpp kTglcCliffTriplanar upload). Samples the merged
// matNormalArray layer 5 (MAT_LAYER_MARBLE_CLIFF, mat5_normal.tga: rgb=normal,
// a=marble_cliff-derived displacement) projected from world axes so vertical
// faces stop showing the stretched top-down splat and read as real rock relief.
// DECOUPLED from the WIP albedo gate MC2_TERRAIN_MATERIAL_TEXTURES: the normal
// array is the always-loaded legacy terrain normal array, so this needs no
// albedo path. Default OFF -> uploads useTriplanarCliff=0 -> frag block no-op
// (byte-identical).
// GATE-REGISTRATION-BACKFILL-1 (terrain-V2 catch-up): register terrain-V2
// arc gates flagged UNREGISTERED by check-new-gates / check-env-registry.
// TERRAIN-CONTROLMAP-SAMPLE-1: authored RGBA control-map override replacing
// the colormap-colour material classifier (frag u_useControlMap).
// history: docs/gate-history.md#MC2_TERRAIN_CONTROLMAP
L(Lead0, "MC2_TERRAIN_CONTROLMAP", Bool, false, "terrain", "", "TERRAIN-CONTROLMAP-SAMPLE-1: load an authored per-mission RGBA control-map sidecar (data/missions/<stem>.beauty/control_map.png, vertex-resolution) that...", passthrough | registry(Feature, "MC2_TERRAIN_CONTROLMAP", -1, false) | verified),
L(Lead0, "MC2_TERRAIN_CONTROLMAP_ALBEDO", Bool, false, "terrain", "", "", passthrough | verified),
P("MC2_TERRAIN_CONTROLMAP_ALBEDO_STRENGTH", Float, 0.0, "terrain", "", "", passthrough | verified),  // default not derivable from the tree
// history: docs/gate-history.md#MC2_TERRAIN_CONTROLMAP_FILE
G("MC2_TERRAIN_CONTROLMAP_FILE", String, "", "terrain", "", "TERRAIN-CONTROLMAP-SAMPLE-1: override the control-map sidecar path (default data/missions/<stem>.beauty/control_map.png).", passthrough | registry(Feature, "MC2_TERRAIN_CONTROLMAP_FILE", -1, false) | verified),
L(Exact1, "MC2_TERRAIN_COST_SPLIT", Bool, false, "terrain", "", "", passthrough | verified),
L(Presence, "MC2_TERRAIN_CULL_WIDE", Bool, false, "terrain", "", "S2.13: MC2_TERRAIN_CULL_WIDE=1 forces the angular sphere cull to admit-all (constants set to 1e9).", verified),
// history: docs/gate-history.md#MC2_TERRAIN_DEBUG_MODE
P("MC2_TERRAIN_DEBUG_MODE", Float, 0.0, "gameos", "", "TERRAIN-DEBUG-VIEWS-1: terrain fragment-shader debug-mode selector for the tessellated terrain path (gos_terrain.frag tessDebug.x).", registry(Trace, "MC2_DIAG_TERRAIN_DEBUG_MODE", -1, false)),  // default not derivable from the tree
L(Lead0, "MC2_TERRAIN_DETAIL_ANTITILE", Bool, false, "terrain", "", "", verified),
P("MC2_TERRAIN_DETAIL_FADE_END", Float, 3000.0, "gameos", "", ""),
P("MC2_TERRAIN_DETAIL_FADE_START", Float, 1000.0, "gameos", "", ""),
P("MC2_TERRAIN_DETAIL_LOD1_STRENGTH", Float, 0.4, "terrain", "", "", verified),
P("MC2_TERRAIN_DETAIL_MACRO_STRENGTH", Float, 0.0, "terrain", "", "", divergent | verified),  // default not derivable from the tree
L(Lead0Kill, "MC2_TERRAIN_EDGE_FEATHER", Bool, true, "terrain", "", "", verified),
P("MC2_TERRAIN_EDGE_FEATHER_STRENGTH", Float, 1.0, "terrain", "", "", rangeAbove(0.0, 3.4028234663852886e38) | verified),
L(Lead0Kill, "MC2_TERRAIN_INDIRECT", Bool, true, "terrain", "", "", passthrough | verified),
L(Lead0Kill, "MC2_TERRAIN_INDIRECT_MINE", Bool, true, "terrain", "", "", passthrough | verified),
L(Lead0Kill, "MC2_TERRAIN_INDIRECT_OVERLAY", Bool, true, "terrain", "", "", passthrough | verified),
L(Exact1, "MC2_TERRAIN_INDIRECT_OVERLAY_PARITY_CHECK", Bool, false, "terrain", "", "", passthrough | verified),
L(Exact1, "MC2_TERRAIN_INDIRECT_PARITY_CHECK", Bool, false, "terrain", "", "", passthrough | verified),
L(Presence, "MC2_TERRAIN_INDIRECT_THINEMIT_TRACE", Bool, false, "terrain", "", "Step 1b-1 (cpu-pack-retirement plan ?2 A1): the M2 thin-record emit scope is orphaned by Fix B (commit 005ebc7) - the GPU compute shader..."),
L(Exact1, "MC2_TERRAIN_INDIRECT_TRACE", Bool, false, "terrain", "", "", passthrough | verified),
L(Lead0Kill, "MC2_TERRAIN_LIGHTING_GPU", Bool, true, "terrain", "", "", passthrough | verified),
// NIGHT-LIGHT-POOLS-1: the LOD-chunk terrain consumes the per-frame terrain
// light pool (gos_terrain_lighting PackAndDispatch light SSBO) per-fragment,
// restoring stock-style night up-lighting on ground + cement pads.
L(Lead0Kill, "MC2_TERRAIN_LOD_CHECKER_DIAG", Bool, true, "terrain", "", "", verified),
// history: docs/gate-history.md#MC2_TERRAIN_LOD_CHUNK
L(Lead0Kill, "MC2_TERRAIN_LOD_CHUNK", Bool, true, "terrain", "", "LOD-chunk terrain renderer (mclib/terrain.cpp/terrain.h), the primary tessellation-free terrain path.", registry(Feature, "MC2_FEATURE_TERRAIN_LOD_CHUNK", -1, true) | verified),
P("MC2_TERRAIN_LOD_CHUNK_CEMENT_MAXLOD", Int, 0, "terrain", "", "", verified),
P("MC2_TERRAIN_LOD_CHUNK_DIAG", Int, 0, "terrain", "", "", passthrough | verified),
P("MC2_TERRAIN_LOD_CHUNK_DIST_SCALE", Float, 1.0, "terrain", "", "", rangeAbove(static_cast<double>(0.05f), 3.4028234663852886e38) | verified),
P("MC2_TERRAIN_LOD_CHUNK_FORCE_LOD", Int, -1, "terrain", "", "", verified),
L(Presence, "MC2_TERRAIN_LOD_CHUNK_NO_APRON", Bool, false, "terrain", "", "", verified),
L(Presence, "MC2_TERRAIN_LOD_CHUNK_NO_CULL", Bool, false, "terrain", "", "Diagnostic: one-shot dump of planes + first superchunk AABB + camera pos.", verified),
L(Presence, "MC2_TERRAIN_LOD_CHUNK_NO_SKIRTS", Bool, false, "terrain", "", "", verified),
L(Presence, "MC2_TERRAIN_LOD_CHUNK_NO_STITCH", Bool, false, "terrain", "", "Phase 10.2b: PER-EDGE mask.", verified),
P("MC2_TERRAIN_LOD_CHUNK_SKIRT_MAX", Float, 256.0, "terrain", "", "", verified),
L(Lead0Kill, "MC2_TERRAIN_LOD_GEOMORPH", Bool, true, "terrain", "", ""),
P("MC2_TERRAIN_LOD_MORPH_START", Float, 0.6, "terrain", "", "", clampRange(0.0, 0.95) | verified),
L(Lead0, "MC2_TERRAIN_MACRO_VARIATION", Bool, false, "terrain", "", "", verified),
P("MC2_TERRAIN_MACRO_VARIATION_STRENGTH", Float, 1.0, "terrain", "", "", rangeAbove(0.0, 3.4028234663852886e38) | verified),
L(Lead0Kill, "MC2_TERRAIN_MASK_DISPATCH_SOLID", Bool, true, "terrain", "", ""),
L(Lead0Kill, "MC2_TERRAIN_MASK_DISPATCH_WATER", Bool, true, "terrain", "", ""),
L(Lead0, "MC2_TERRAIN_MATERIAL_LIB", Bool, false, "terrain", "", "", passthrough | verified),
G("MC2_TERRAIN_MATERIAL_LIB_FILE", String, "", "terrain", "", "", passthrough | verified),  // default not derivable from the tree
// TERRAIN-MATERIAL-TEXTURES-1: per-layer PBR albedo textures on the LIVE
// LOD-chunk terrain path (gos_terrain_lod_chunk.cpp lazy loader + binder,
// terrain_lod_chunk.frag u_useMatAlbedo branch). Gate OFF -> u_useMatAlbedo
// uploads 0 -> the colormap-tint composition runs VERBATIM (byte-identical).
// history: docs/gate-history.md#MC2_TERRAIN_MATERIAL_TEXTURES
L(Lead0, "MC2_TERRAIN_MATERIAL_TEXTURES", Bool, false, "terrain", "", "TERRAIN-MATERIAL-TEXTURES-1: 6-layer BC7 sRGB GL_TEXTURE_2D_ARRAY albedo (rock/grass/dirt/concrete/snow/cliff from...", passthrough | registry(Feature, "MC2_FEATURE_TERRAIN_MATERIAL_TEXTURES", -1, false) | verified),
// history: docs/gate-history.md#MC2_TERRAIN_MATERIAL_TEXTURES_STRENGTH
P("MC2_TERRAIN_MATERIAL_TEXTURES_STRENGTH", Float, -1.0, "terrain", "", "TERRAIN-MATERIAL-TEXTURES-1 knob: albedo mix strength override (float, clamped 0..1).", passthrough | clampRange(0.0, 1.0) | registry(Trace, "MC2_TUNE_TERRAIN_MATERIAL_TEXTURES_STRENGTH", -1, false)),
L(Presence, "MC2_TERRAIN_MINE_AB", Bool, false, "terrain", "", "", verified),
L(Lead0Kill, "MC2_TERRAIN_NORMAL_ARRAY", Bool, true, "gameos", "", "", verified),
// TERRAIN-OVERLAY-V2-PARITY-1: authored cement/pad/runway overlay sidecar.
// history: docs/gate-history.md#MC2_TERRAIN_OVERLAY_V2
L(Lead0, "MC2_TERRAIN_OVERLAY_V2", Bool, false, "terrain", "", "TERRAIN-OVERLAY-V2-PARITY-1: composite an authored world-XY-bounded cement/pad/runway overlay sidecar (data/missions/<stem>.beauty/overlay_v2.png...", passthrough | registry(Feature, "MC2_TERRAIN_OVERLAY_V2", -1, false) | verified),
// TERRAIN-OVERLAY-V2-DECAL-SUPPRESS-1.
// history: docs/gate-history.md#MC2_TERRAIN_OVERLAY_V2_DECAL_SUPPRESS
L(Lead0Kill, "MC2_TERRAIN_OVERLAY_V2_DECAL_SUPPRESS", Bool, true, "terrain", "", "TERRAIN-OVERLAY-V2-DECAL-SUPPRESS-1: in the static decal bake, skip legacy runway/road/cement decals for tiles fully inside the armed OVERLAY_V2 sidecar...", registry(Feature, "MC2_TERRAIN_OVERLAY_V2_DECAL_SUPPRESS", -1, true) | verified),
// history: docs/gate-history.md#MC2_TERRAIN_OVERLAY_V2_FILE
G("MC2_TERRAIN_OVERLAY_V2_FILE", String, "", "terrain", "", "TERRAIN-OVERLAY-V2-PARITY-1: override the overlay_v2 sidecar path (default data/missions/<stem>.beauty/overlay_v2.png).", passthrough | registry(Feature, "MC2_TERRAIN_OVERLAY_V2_FILE", -1, false) | verified),
L(Presence, "MC2_TERRAIN_PICK_PARITY", Bool, false, "code", "", "PARITY DIAGNOSTIC: when MC2_TERRAIN_PICK_PARITY is set we compute BOTH the heightfield raycast hit AND the legacy quadList result for the same screen...", verified),
// TERRAIN-CHUNK-POM-1: real view-vector parallax occlusion mapping on the
// LIVE LOD-chunk terrain path (gos_terrain_lod_chunk.cpp uploads,
// terrain_lod_chunk.frag chunkParallaxView). Gate OFF preserves the legacy
// faux-view-vector chunkParallax output VERBATIM (supervisor ruling:
// byte-identity INCLUDES the faux shear — pomParams is NOT zeroed when OFF).
L(Presence, "MC2_TERRAIN_PROBE", Bool, false, "editor", "", "", verified),
L(Presence, "MC2_TERRAIN_RAYCAST_PICK", Bool, false, "code", "", "Phase 7B: heightfield raycast picker - replaces the quadList AABB/ screen-triangle scan when MC2_TERRAIN_LOD_CHUNK=1.", verified),
L(Exact1, "MC2_TERRAIN_RUNTIME_DECALS", Bool, false, "terrain", "", "", verified),
L(Exact1, "MC2_TERRAIN_RUNTIME_GROUNDING", Bool, false, "terrain", "", "", verified),
L(Lead1, "MC2_TERRAIN_RUNTIME_PARITY", Bool, false, "terrain", "", "", verified),
// history: docs/gate-history.md#MC2_TERRAIN_SHADOW_GUARD_CLAMPED_SUN
L(Lead0Kill, "MC2_TERRAIN_SHORELINE", Bool, true, "terrain", "", "", passthrough | verified),
