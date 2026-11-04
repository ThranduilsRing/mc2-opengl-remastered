P("MC2_FPS_CAP", Int, 90, "gameos", "", "MC2_FPS_CAP: max FPS override.", passthrough | verified),
L(Presence, "MC2_FRAMECTX_MISMATCH_FATAL", Bool, false, "gameos", "", "", verified),
L(Presence, "MC2_FRAMEGRAPH_AMBIENT_FATAL", Bool, false, "code", "", "", verified),
L(Lead1, "MC2_FRAMEGRAPH_DRYRUN", Bool, false, "code", "", "", passthrough | verified),
// history: docs/gate-history.md#MC2_FRAMEGRAPH_EXECUTOR
L(Lead0Kill, "MC2_FRAMEGRAPH_EXECUTOR", Bool, true, "post", "", "RENDER-FRAME-GRAPH arc: top-level frame-graph executor ownership of pass begin/end (mclib/render_contract.cpp, GameOS/gameos/gos_postprocess.cpp).", passthrough | registry(Feature, "MC2_FEATURE_FRAMEGRAPH_EXECUTOR", -1, true) | verified),
L(AtoiNZ, "MC2_FRAME_JOBS", Bool, false, "code", "", "", passthrough | verified),
P("MC2_FRAME_JOBS_BATCH", Int, 64, "code", "", "", passthrough | verified),
P("MC2_FRAME_JOBS_WORKERS", Int, 0, "code", "", "", passthrough | verified),  // default not derivable from the tree
// history: docs/gate-history.md#MC2_FREEZE_MOVERS_AT
// history: docs/gate-history.md#MC2_FX_COST_SPLIT
// FX-DEFS-SIDECAR-1: EffectDef JSON overlay (VFX-MODERNIZATION-PROPOSAL-1 slice 1)
// history: docs/gate-history.md#MC2_FX_DEFS
// FX-FORCE-SPAWN fixture (mech-fire tube)
// history: docs/gate-history.md#MC2_FX_FORCE_SPAWN
G("MC2_FX_FORCE_SPAWN", Enum, "", "code", "", "FX-FORCE-SPAWN v1 fixture: default-OFF. =impact: impact-decal volley.", passthrough | registry(Trace, "MC2_FX_FORCE_SPAWN", -1, false) | tokens("|0|1|2|3|4|5|6|7|8|9|loop|impact") | verified),
L(Lead1, "MC2_FX_TRACE", Bool, false, "code", "", "", passthrough | verified),

P("MC2_GLTF_AXIS", Int, 0, "props", "", "Coordinate transforms (spec ?6).", reread | verified),
P("MC2_GLTF_YAW_DEG", Float, 0.0, "props", "", "", reread | verified),
L(Presence, "MC2_GOM_RECON", Bool, false, "code", "", "GOM Recon: object type classification & mutation tracking (read-only).", verified),
// GOM_UPDATE_COST v1: per-phase GameObjectManager update CPU cost trace.
// history: docs/gate-history.md#MC2_GOM_UPDATE_COST_SPLIT
// history: docs/gate-history.md#MC2_GPUBUF_LIGHT_GROWONCE
// history: docs/gate-history.md#MC2_GPUBUF_RESIDENCY
// history: docs/gate-history.md#MC2_GPUBUF_RING
// history: docs/gate-history.md#MC2_GPUBUF_RING_FORCE_WRAP
P("MC2_GPU_BUDGET_MB", Int, 0, "gameos", "", "Diagnostic residency ceiling in MiB; zero uses device/preset policy. Cannot raise the device budget.", passthrough | verified),
L(Lead0Kill, "MC2_GPU_CULL", Bool, true, "unclassified", "", "", passthrough | verified),
L(Presence, "MC2_GPU_CULL_STATIC_EAGER_LIGHT_BAKE", Bool, false, "props", "", "", verified),
L(Presence, "MC2_GPU_CULL_STATIC_FROZEN_RECORDS", Bool, false, "props", "", "M1 FROZEN-STATIC-CULL-RECORDS (gate MC2_GPU_CULL_STATIC_FROZEN_RECORDS).", verified),
// GATE-HYGIENE-CLOSE-1: static-prop light-bake probe (was legacy-allowlist-escaped).
// history: docs/gate-history.md#MC2_GPU_CULL_STATIC_LIGHT_ZERO_PROBE
L(Lead0Kill, "MC2_GPU_CULL_SUBSTRATE", Bool, true, "gameos", "", "Default ON; MC2_GPU_CULL_SUBSTRATE=0 opts out.", passthrough | verified),
L(Lead0Kill, "MC2_GPU_DRIVEN", Bool, true, "unclassified", "", "", passthrough | verified),
L(Lead0Kill, "MC2_GPU_DRIVEN_WATER", Bool, true, "gameos", "", "", passthrough | verified),
G("MC2_GPU_GROUND_PICK_PARITY_TAG", String, "", "code", "", "", passthrough | verified),  // default not derivable from the tree
// ENV sweep 2026-07-09 (checker regex widen: digit names + envFlagDefaultOn helper)
// MC2_GPU_PARTICLES (master GPU-particle path; lives in kAuxEnvVars: kFeatureTable is indexed by RendererFeature; read in mclib/particles/batcher.cpp initialize_env)
// history: docs/gate-history.md#MC2_GPU_PARTICLES
L(DefOnLead0, "MC2_GPU_PARTICLES", Bool, true, "code", "", "Native Card/CardCloud rendering; =0 suppresses these draws (CPU routes retired).", passthrough | registry(Feature, "MC2_GPU_PARTICLES", -1, true) | verified),
L(Lead1, "MC2_GPU_PARTICLES_LOG", Bool, false, "code", "", "", passthrough | verified),
// GvLightsOutClear (VEHICLE-NIGHT-LIT-2)
L(Lead0Kill, "MC2_HDRI_SKY", Bool, true, "post", "", "", verified),
P("MC2_HDRI_SKY_AZ_OFFSET", Float, 0.0, "post", "", "", verified),
L(Presence, "MC2_HEARTBEAT", Bool, false, "gameos", "", "", passthrough | verified),
P("MC2_HEIGHT", Int, 0, "gameos", "", "", passthrough | verified),
// history: docs/gate-history.md#MC2_HUD_IMGUI_TABS
L(Lead0, "MC2_HUD_IMGUI_TABS", Bool, false, "code", "", "IN-MISSION-HUD-IMGUI-SCOPE-1 slice C: data-driven command-bar tab registry (code/hudtabregistry.{h,cpp} + wiring in code/controlgui.cpp).", registry(Feature, "MC2_FEATURE_HUD_IMGUI_TABS", -1, false) | verified),
// history: docs/gate-history.md#MC2_HUD_READABILITY_V1
L(Lead0, "MC2_HUD_READABILITY_V1", Bool, false, "unclassified", "", "IN-MISSION-HUD-IMGUI-SCOPE-1 slice A: data-first HUD readability.", registry(Feature, "MC2_FEATURE_HUD_READABILITY_V1", -1, false) | verified),
P("MC2_HUD_SCALE", Float, 0.0, "gameos", "", "", verified),  // default not derivable from the tree
L(Lead0, "MC2_HUD_SIM_SPEED", Bool, false, "code", "", "SIM-SPEED-BUTTON-1: double-gated sim-speed tab (MC2_HUD_SIM_SPEED; needs MC2_HUD_IMGUI_TABS like every registry consumer).", verified),  // default not derivable from the tree
// history: docs/gate-history.md#MC2_HUD_TABS_TEST
// history: docs/gate-history.md#MC2_HZB_BUILD
G("MC2_HZB_BUILD", Bool, true, "post", "", "Render-only static-prop culling from same-frame Vulkan depth; exact ceil-ladder MIN footprints and per-object LOD. MC2_HZB_BUILD=0 keeps every instance.", passthrough | registry(Feature, "MC2_FEATURE_HZB_BUILD", 17, true) | verified),
G("MC2_HZB_VIEW_FILE", String, "", "code", "", "HZB validation knob (debug only): force a low across-map HORIZON view so static props occlude each other.", verified),  // default not derivable from the tree
L(Lead1, "MC2_IBL_ASSET_STRICT", Bool, false, "unclassified", "", ""),
// history: docs/gate-history.md#MC2_IFACE_COST_SPLIT
// history: docs/gate-history.md#MC2_IMGUI
L(Lead0Kill, "MC2_IMGUI", Bool, true, "code", "", "ImGui overlay (GuiRuntime/GuiRuntime.cpp).", registry(Feature, "MC2_FEATURE_IMGUI", 6, true) | verified),
L(Lead1, "MC2_IMGUI_DEMO", Bool, false, "code", "", "", verified),
// history: docs/gate-history.md#MC2_IMGUI_INSPECTOR
L(Lead0Kill, "MC2_IMGUI_INSPECTOR", Bool, true, "code", "", "ImGui inspector panel (GuiRuntime/EditorInspector.cpp).", registry(Feature, "MC2_FEATURE_IMGUI_INSPECTOR", 7, true) | verified),
G("MC2_IMPORTER_DIR", String, "", "unclassified", "", ""),
G("MC2_INPUT_SCRIPT", String, "", "code", "", "Player-path harness only: file of scripted input commands (click/key/wait) fed into the engine input state; inert when unset.", passthrough | verified),
L(Presence, "MC2_JUMPJETS_ALL", Bool, false, "code", "", "", verified),
L(Presence, "MC2_LAUNCHED", Bool, false, "gameos", "", "", verified),
L(Lead0Kill, "MC2_LAUNCHER_ENV_JSON", Bool, true, "gameos", "", "", verified),
// LegacyLitMc2Frame (VEHICLE-NIGHT-LIT-1)
// history: docs/gate-history.md#MC2_LEGACY_LIT_MC2_FRAME
// LetterboxOpaque (DETERMINISM-RATCHET-2)
// LIGHTBAKE-REBAKE-ON-READY-1: one-shot static-light re-bake after mission ready.
// history: docs/gate-history.md#MC2_LIGHTBAKE_REBAKE_ON_READY
L(Lead0Kill, "MC2_LIGHTBAKE_REBAKE_ON_READY", Bool, true, "props", "", "LIGHTBAKE-REBAKE-ON-READY-1: soft-invalidate the per-recipe static-light bake cache once, 8 frames after mission-ready, so recipes baked during boot/load...", passthrough | registry(Feature, "MC2_FEATURE_LIGHTBAKE_REBAKE_ON_READY", -1, true) | verified),
// STATICPROP-PERMANENT-INSTANCE-LIGHTS-1 (Slice 1)
// PERF-HARNESS-TRUTH-1: shipped unregistered, undocumented AND absent from
// the run_smoke.py passthrough allowlist, so it silently no-opped through
// the standard harness -- a lighting investigation could set it, see the
// ordinary lit render, and conclude the debug view showed nothing wrong.
// not Trace: it REPLACES the lit render output
G("MC2_LIGHTING_SHADOW_PREDICATE_MODE", Enum, "", "code", "", "", tokens("|Legacy|Modern") | verified),
G("MC2_LIGHT_BILLBOARD_FACING", String, "", "props", "", "", passthrough | verified),
L(Exact1, "MC2_LIGHT_CLAMP_FIXTURE", Bool, false, "code", "", "MC2_LIGHT_CLAMP_FIXTURE (default OFF) - synthetic injector of deterministic extra point lights into slots [populated..32) to prove slots >16 ever render.", verified),
P("MC2_LIGHT_CONE_GLOW_ALPHA", Float, 40.0, "code", "", "", passthrough | clampRange(0.0, 255.0) | verified),
// history: docs/gate-history.md#MC2_LIGHT_CONE_NODES
P("MC2_LIGHT_CONE_SPILL", Float, 1.0, "code", "", "", passthrough | clampRange(0.0, 3.0) | verified),
// LIGHT-CLUSTER-PERF-1: recover the frame time LIGHT-POOL-CAP-1 spent when it
// raised the pool cap 64 -> 256 (the fix was correct -- mc2_10 wants 142
// fixtures -- but the consumer loop is per-fragment, so cost scaled with the
// whole pool).
// history: docs/gate-history.md#MC2_LIGHT_GRID
L(Lead0Kill, "MC2_LIGHT_GRID", Bool, true, "terrain", "", "LIGHT-CLUSTER-PERF-1: world-space XY light GRID over the per-frame terrain light pool.", passthrough | verified | registry(Feature, "MC2_FEATURE_LIGHT_GRID", -1, true)),
// history: docs/gate-history.md#MC2_LIGHT_GRID_DIM
P("MC2_LIGHT_GRID_DIM", Int, 64, "terrain", "", "LIGHT-CLUSTER-PERF-1 knob: cells per axis for the light grid (grid is DIM x DIM over the XY bbox of this frame's packed lights).", passthrough | range(2, 256) | verified | registry(Trace, "MC2_TUNE_LIGHT_GRID_DIM", -1, false)),
// NIGHT-LIGHT-POOLS-3: overlapping pool sums are tone-compressed instead of
// hard-clamped (5-mech group pools no longer flat-top a saturated blob).
// history: docs/gate-history.md#MC2_LIGHT_POOL_COMPRESS
L(Lead0Kill, "MC2_LIGHT_POOL_COMPRESS", Bool, true, "terrain", "", "NIGHT-LIGHT-POOLS-3: tone-compress the accumulated terrain light-pool sum (chunk + overlay frag): 1-exp(-1.6*sum) instead of min(sum,1).", passthrough | verified | registry(Feature, "MC2_FEATURE_LIGHT_POOL_COMPRESS", -1, true)),
// LIGHT-POOL-CAP-1 (5213d257) shipped this override unregistered and
// undocumented -- registered here by LIGHT-CLUSTER-PERF-1's meta review.
// history: docs/gate-history.md#MC2_LIGHT_POOL_MAX
P("MC2_LIGHT_POOL_MAX", Int, 0, "terrain", "", "LIGHT-POOL-CAP-1: per-mission slot count of the terrain light pool (gos_terrain_lighting.cpp mission_init).", passthrough | range(0, 4096) | verified | registry(Trace, "MC2_TUNE_LIGHT_POOL_MAX", -1, false)),
G("MC2_LIGHT_POOL_MOVER_COLOR", String, "", "code", "", "MOVER-LIGHT-HALOGEN-1: hex RRGGBB mover light colour; unset, empty or malformed keeps FFEED1.", passthrough | verified),
P("MC2_LIGHT_POOL_MOVER_DIM", Float, 0.25, "terrain", "", "", passthrough | range(0.0, 1.0) | verified),
P("MC2_LIGHT_POOL_MOVER_FWD", Float, 24.0, "code", "", "", passthrough | clampRange(0.0, 120.0) | verified),
P("MC2_LIGHT_POOL_MOVER_RADIUS", Float, 0.6, "terrain", "", "", passthrough | rangeAbove(0.0, 1.0) | verified),
P("MC2_LIGHT_POOL_OBJ_SCALE", Float, 0.7, "terrain", "", "", passthrough | clampRange(0.0, 2.0) | verified),
// LightPosColumnFix (LIGHT-POS-COLUMN-1)
// history: docs/gate-history.md#MC2_LIGHT_POS_COLUMN_FIX
L(Lead0Kill, "MC2_LIGHT_POS_COLUMN_FIX", Bool, true, "code", "", "[DEFAULT-ON as of VEHICLE-LIGHTING-PATH-1; MC2_LIGHT_POS_COLUMN_FIX=0 reverts.", passthrough | registry(Feature, "MC2_LIGHT_POS_COLUMN_FIX", 23, true) | verified),
// history: docs/gate-history.md#MC2_LIGHT_PREFIX_GPU_COPY
// PERF-HARNESS-TRUTH-1: registered while measuring its cost. It was
// allowlisted for smoke but neither registered nor documented, so the gate
// holding ~6 reported visual items had no discoverable contract.
// history: docs/gate-history.md#MC2_LIGHT_REALISM
L(Lead0Kill, "MC2_LIGHT_REALISM", Bool, true, "terrain", "", "LIGHT-REALISM-1: night light-pool + cone-glow realism reshape.", passthrough | verified | registry(Feature, "MC2_FEATURE_LIGHT_REALISM", -1, true)),
P("MC2_LIGHT_REALISM_BEACON", Float, 70.0, "code", "", "", passthrough | clampRange(0.0, 255.0) | verified),
P("MC2_LIGHT_REALISM_BEACON_RADIUS", Float, 40.0, "code", "", "", passthrough | clampRange(1.0, 200.0) | verified),
P("MC2_LIGHT_REALISM_CONE_FEATHER", Float, 2.5, "code", "", "", passthrough | clampRange(0.0, 8.0) | verified),
P("MC2_LIGHT_REALISM_CONE_OPACITY", Float, 0.4, "code", "", "", passthrough | clampRange(0.0, 4.0) | verified),
P("MC2_LIGHT_REALISM_POOL_CORE", Float, 0.0, "terrain", "", "", passthrough | clampRange(0.0, 1.0) | verified),
P("MC2_LIGHT_REALISM_POOL_SPILL", Float, 2.2, "terrain", "", "", passthrough | clampRange(0.25, 4.0) | verified),
// LightRowPure (LIGHT-ROW-PURE-1, ARCH-LIGHTING slice 1)
// kFeatureTable is indexed by RendererFeature, so this entry's position is
// its enum value (57).
// history: docs/gate-history.md#MC2_LIGHT_ROW_PURE
// ── LIGHT-ROW-PURE-1 ───────────────────────────────────────────────────
// The in-engine half of the byte-identity proof for MC2_LIGHT_ROW_PURE.
// Lives here rather than in kFeatureTable because it is a Trace var and
// gates no render path -- it only observes.
// history: docs/gate-history.md#MC2_LIGHT_ROW_PURE_VERIFY
// history: docs/gate-history.md#MC2_LINKUP_TRACE
// history: docs/gate-history.md#MC2_LOAD_RACE_DELAY_ATLAS
// history: docs/gate-history.md#MC2_LOAD_RACE_PROBE_FRAMES
G("MC2_LOAD_RACE_PROBE_FRAMES", String, "", "gameos", "", "gameosmain.cpp: overrides the comma-separated list of frame numbers at which the mission-load residency-race one-shot diagnostics probe fires (see...", registry(Trace, "MC2_TRACE_LOAD_RACE_PROBE_FRAMES", -1, false) | verified),
P("MC2_LOAD_RACE_VERDICT_FRAME", Int, 0, "gameos", "", "", verified),  // default not derivable from the tree
// MC2_LOG master log gate
// history: docs/gate-history.md#MC2_LOG
G("MC2_LOG", String, "", "gameos", "", "Master stdout log gate (gameosmain.cpp, right after setvbuf).", passthrough | registry(Trace, "MC2_LOG", -1, false) | verified),
G("MC2_LOG_DECORATE", String, "", "gameos", "", "", verified),  // default not derivable from the tree
// history: docs/gate-history.md#MC2_LOG_FILE_RESOLVE
G("MC2_LOG_FILE_RESOLVE", String, "", "code", "", "META-FIX design E tier 3 (mclib/file.cpp, parsed at InitModSearchPaths): file-resolution ladder diagnostics.", passthrough | registry(Trace, "MC2_TRACE_LOG_FILE_RESOLVE", -1, false) | verified),
