L(Presence, "MC2_FORCE_TXM_REGEN", Bool, false, "terrain", "", "MC2_FORCE_TXM_REGEN=1 -> ignore the pre-cooked .txm composite cache and regenerate terrain texture composites from the source .tga (cement_N, overlays...", verified),
P("MC2_FPS_CAP", Int, 90, "gameos", "", "MC2_FPS_CAP: max FPS override.", passthrough | verified),
L(Presence, "MC2_FRAMECAP_TRACE", Bool, false, "mech", "", "", verified),
L(Presence, "MC2_FRAMECTX_MISMATCH_FATAL", Bool, false, "gameos", "", "", verified),
L(Presence, "MC2_FRAMEGRAPH_AMBIENT_FATAL", Bool, false, "code", "", "", verified),
L(Lead0Kill, "MC2_FRAMEGRAPH_AMBIENT_GUARD", Bool, true, "code", "", "", verified),
L(Lead1, "MC2_FRAMEGRAPH_DRYRUN", Bool, false, "code", "", "", passthrough | verified),
// history: docs/gate-history.md#MC2_FRAMEGRAPH_EXECUTOR
L(Lead1, "MC2_FRAMEGRAPH_EXECUTOR", Bool, false, "post", "", "RENDER-FRAME-GRAPH arc: top-level frame-graph executor ownership of pass begin/end (mclib/render_contract.cpp, GameOS/gameos/gos_postprocess.cpp).", passthrough | registry(Feature, "MC2_FEATURE_FRAMEGRAPH_EXECUTOR", -1, false) | verified),
L(Presence, "MC2_FRAMEGRAPH_REORDER_SPMECH", Bool, false, "code", "", "MEASURED-REORDER-SPMECH-1 (tier-C reorder EXPERIMENT, default-OFF): The two opaque flush UNITS below - StaticProp BatcherFlush (+Pass_SpColor timer) and the...", passthrough | verified),
L(AtoiNZ, "MC2_FRAME_JOBS", Bool, false, "code", "", "", passthrough | verified),
P("MC2_FRAME_JOBS_BATCH", Int, 64, "code", "", "", passthrough | verified),
L(Lead1, "MC2_FRAME_JOBS_TOUCH", Bool, false, "code", "", "", passthrough | verified),
G("MC2_FRAME_JOBS_TOUCH_DIAG", Bool, false, "props", "", "Std Bool row: any text other than 0/false/off/no/1/true/on/yes (e.g. =2) still reads as on but is reported badvalue, which MC2_CONFIG_STRICT=1 turns fatal.", verified | passthrough),
L(AtoiNZ, "MC2_FRAME_JOBS_TRACE", Bool, false, "code", "", "", passthrough | verified),
P("MC2_FRAME_JOBS_WORKERS", Int, 0, "code", "", "", passthrough | verified),  // default not derivable from the tree
L(Exact1, "MC2_FRAME_PASS_STATS", Bool, false, "gameos", "", "", passthrough | verified),
P("MC2_FRAME_PASS_STATS_EVERY", Int, 0, "gameos", "", "", passthrough | verified),
// history: docs/gate-history.md#MC2_FREEZE_MOVERS_AT
P("MC2_FREEZE_MOVERS_AT", Int, -1, "code", "", "FREEZE-MOVERS-1: =<frame> freezes every mover the first frame g_mc2FrameCounter >= <frame>, WITHOUT needing the command channel (the 'freeze_movers on...", passthrough | registry(Trace, "MC2_TRACE_FREEZE_MOVERS_AT", -1, false) | verified),
P("MC2_FXAA_EDGE_THRESHOLD", Float, 0.0, "post", "", "", passthrough),  // default not derivable from the tree
P("MC2_FXAA_EDGE_THRESHOLD_MIN", Float, 0.0, "post", "", "", passthrough),  // default not derivable from the tree
P("MC2_FXAA_SUBPIX", Float, 0.0, "post", "", "", passthrough),  // default not derivable from the tree
// history: docs/gate-history.md#MC2_FX_COST_SPLIT
L(Exact1, "MC2_FX_COST_SPLIT", Bool, false, "code", "", "FX cost-split instrumentation (default-off).", passthrough | registry(Feature, "MC2_FX_COST_SPLIT", -1, false) | verified),
L(Presence, "MC2_FX_COUNT_LOG", Bool, false, "mech", "", "MC2_FX_COUNT_LOG=1: env-gated per-effect-site GOSFX draw counter for the Mech3DAppearance::render() effect sites.", passthrough | verified),
// FX-DEFS-SIDECAR-1: EffectDef JSON overlay (VFX-MODERNIZATION-PROPOSAL-1 slice 1)
// history: docs/gate-history.md#MC2_FX_DEFS
L(Presence, "MC2_FX_DEFS", Bool, false, "mech", "", "FX-DEFS-SIDECAR-1: per-effect JSON sidecar overlay for the gosFX spec catalog (mc2::particles::SpecLibrary).", passthrough | registry(Feature, "MC2_FX_DEFS", -1, false) | verified),
// FX-FORCE-SPAWN fixture (mech-fire tube)
// history: docs/gate-history.md#MC2_FX_FORCE_SPAWN
G("MC2_FX_FORCE_SPAWN", Enum, "", "code", "", "FX-FORCE-SPAWN v1 fixture: default-OFF. =impact: impact-decal volley.", passthrough | registry(Feature, "MC2_FX_FORCE_SPAWN", -1, false) | tokens("|0|1|2|3|4|5|6|7|8|9|loop|impact") | verified),
L(Lead1, "MC2_FX_TRACE", Bool, false, "code", "", "", passthrough | verified),

G("MC2_GAMEOS_UI_DEFS", Bool, true, "editor", "", "The data/defs GameOS UI path is the replacement path, not an opt-in demo.", verified),
L(Exact1, "MC2_GAMEPLAY_PICK_SELFTEST", Bool, false, "code", "", "", verified),
L(Presence, "MC2_GEOM_PHASE_SPLIT", Bool, false, "terrain", "", "", passthrough | verified),
P("MC2_GLTF_AXIS", Int, 0, "props", "", "Coordinate transforms (spec ?6).", reread | verified),
P("MC2_GLTF_GROUND", Int, 2, "code", "", "", verified),
P("MC2_GLTF_YAW_DEG", Float, 0.0, "props", "", "", reread | verified),
P("MC2_GLTF_YOFF", Float, 0.0, "code", "", "", verified),
L(Presence, "MC2_GOM_RECON", Bool, false, "code", "", "GOM Recon: object type classification & mutation tracking (read-only).", verified),
// GOM_UPDATE_COST v1: per-phase GameObjectManager update CPU cost trace.
// history: docs/gate-history.md#MC2_GOM_UPDATE_COST_SPLIT
L(Lead0P, "MC2_GOM_UPDATE_COST_SPLIT", Bool, false, "code", "", "GOM_UPDATE_COST v1 (framebudget): =1 emits one coarse per-phase GameObjectManager update CPU-cost summary to stderr (objmgr.cpp).", passthrough | registry(Trace, "MC2_GOM_UPDATE_COST_SPLIT", -1, false) | verified),
L(Lead1, "MC2_GOSFX_GROUP_LOG", Bool, false, "gameos", "", "", verified),
L(Presence, "MC2_GPUBUF_COUNTER", Bool, false, "gameos", "", "GPU-UPDATE-BUFFER-COUNTER-1: independent default-OFF gate.", verified),
// history: docs/gate-history.md#MC2_GPUBUF_LIGHT_GROWONCE
// history: docs/gate-history.md#MC2_GPUBUF_RESIDENCY
// history: docs/gate-history.md#MC2_GPUBUF_RING
// history: docs/gate-history.md#MC2_GPUBUF_RING_FORCE_WRAP
L(Presence, "MC2_GPUPROPS_TRACE", Bool, false, "props", "", "[GPUPROPS v1] setup-path instrumentation (internal; path tag lives at file scope).", passthrough | verified),
L(Lead0Kill, "MC2_GPU_CULL", Bool, true, "unclassified", "", "", passthrough | verified),
L(Lead0P, "MC2_GPU_CULL_AABB_PARITY", Bool, false, "gameos", "", "", passthrough | verified),
L(Lead0Kill, "MC2_GPU_CULL_CONSERVATIVE_OR", Bool, true, "gameos", "", "", verified),
L(Presence, "MC2_GPU_CULL_FORCE_FENCE_NOT_READY", Bool, false, "gameos", "", "", passthrough | verified),
P("MC2_GPU_CULL_FRUSTUM_DILATION", Float, 0.08, "gameos", "", "Frustum dilation of the actor compute cull; outside [0, 1] reads the default.", range(0.0, 1.0) | verified),
L(Presence, "MC2_GPU_CULL_LIFECYCLE", Bool, false, "mech", "", "C3: env-gated lifecycle routing killswitch (same env var as objmgr.cpp).", passthrough | verified),
L(Presence, "MC2_GPU_CULL_LIFECYCLE_TRACE", Bool, false, "mech", "", "MC2_GPU_CULL_LIFECYCLE_TRACE=1: verbose per-actor lifecycle boundary prints.", passthrough | verified),
L(Lead0Kill, "MC2_GPU_CULL_READBACK", Bool, true, "gameos", "", "PERF-OBJECT-ITER-GPU-PORT-1: default-ON now that substrate+coalesce are stable.", passthrough | verified),
L(Presence, "MC2_GPU_CULL_READBACK_TRACE", Bool, false, "gameos", "", "Env-gated trace macro.", passthrough | verified),
L(Presence, "MC2_GPU_CULL_STATIC_EAGER_LIGHT_BAKE", Bool, false, "props", "", "", verified),
L(Presence, "MC2_GPU_CULL_STATIC_FROZEN_ORDER_ORACLE", Bool, false, "props", "", "Order-validation oracle (gated): for each placed record, verify its actor center is near the recipe's own leaf world position (within the bounding sphere).", verified),
L(Presence, "MC2_GPU_CULL_STATIC_FROZEN_RECORDS", Bool, false, "props", "", "M1 FROZEN-STATIC-CULL-RECORDS (gate MC2_GPU_CULL_STATIC_FROZEN_RECORDS).", verified),
// GATE-HYGIENE-CLOSE-1: static-prop light-bake probe (was legacy-allowlist-escaped).
// history: docs/gate-history.md#MC2_GPU_CULL_STATIC_LIGHT_ZERO_PROBE
L(Presence, "MC2_GPU_CULL_STATIC_LIGHT_ZERO_PROBE", Bool, false, "props", "", "Static-prop light-bake observation gate, default OFF.", registry(Trace, "MC2_TRACE_GPU_CULL_STATIC_LIGHT_ZERO_PROBE", -1, false) | verified),
L(Lead0Kill, "MC2_GPU_CULL_SUBSTRATE", Bool, true, "gameos", "", "Default ON; MC2_GPU_CULL_SUBSTRATE=0 opts out.", passthrough | verified),
L(Presence, "MC2_GPU_CULL_SUBSTRATE_TRACE", Bool, false, "gameos", "", "MC2_GPU_CULL_SUBSTRATE_TRACE=1 - verbose per-frame lifecycle prints.", passthrough | verified),
L(Lead0Kill, "MC2_GPU_DRIVEN", Bool, true, "unclassified", "", "", passthrough | verified),
L(Lead0Kill, "MC2_GPU_DRIVEN_OVERLAY", Bool, true, "gameos", "", "", passthrough | verified),
L(Lead1, "MC2_GPU_DRIVEN_PARITY", Bool, false, "gameos", "", "", passthrough | verified),
L(Lead0Kill, "MC2_GPU_DRIVEN_TERRAIN_SOLID", Bool, true, "gameos", "", "", passthrough | verified),
L(Lead1, "MC2_GPU_DRIVEN_TRACE", Bool, false, "gameos", "", "", passthrough | verified),
L(Lead0Kill, "MC2_GPU_DRIVEN_WATER", Bool, true, "gameos", "", "", passthrough | verified),
L(Lead0, "MC2_GPU_GROUND_PICK_PARITY", Bool, false, "code", "", "", passthrough | verified),
L(Presence, "MC2_GPU_GROUND_PICK_PARITY_CENTER", Bool, false, "code", "", "Center-probe knob: unattended smoke parks the cursor at (0,0), so the primary cursor-driven oracle never samples real terrain (compared=0).", verified),
P("MC2_GPU_GROUND_PICK_PARITY_SAMPLE", Int, 30, "code", "", "Sample every Nth frame for the ground-pick parity oracle; below 1 reads the default 30.", passthrough | range(1.0, 2147483647.0) | verified),
G("MC2_GPU_GROUND_PICK_PARITY_TAG", String, "", "code", "", "", passthrough | verified),  // default not derivable from the tree
L(Lead0Kill, "MC2_GPU_GV_SHADOW_SKIP", Bool, true, "mech", "", "", verified),
// history: docs/gate-history.md#MC2_GPU_MECHS
L(Lead0Kill, "MC2_GPU_MECHS", Bool, true, "mech", "", "[RETIRED] GPU mech batcher (gos_mech_batcher).", registry(Retired, "MC2_FEATURE_GPU_MECHS", 0, true) | verified),
// ENV sweep 2026-07-09 (checker regex widen: digit names + envFlagDefaultOn helper)
// history: docs/gate-history.md#MC2_GPU_OBJECTS
L(Lead0Kill, "MC2_GPU_OBJECTS", Bool, true, "unclassified", "", "[RETIRED] Indirect-draw static-prop batcher (gos_static_prop_batcher).", passthrough | registry(Retired, "MC2_FEATURE_STATIC_PROP_INDIRECT", 1, true) | verified),
// MC2_GPU_PARTICLES (master GPU-particle path; lives in kAuxEnvVars: kFeatureTable is indexed by RendererFeature; read in mclib/particles/batcher.cpp initialize_env)
// history: docs/gate-history.md#MC2_GPU_PARTICLES
L(Lead1, "MC2_GPU_PARTICLES", Bool, false, "code", "", "Master gate for GPU billboard particles (mclib/particles/batcher.cpp initialize_env).", passthrough | registry(Feature, "MC2_GPU_PARTICLES", -1, false) | verified),
L(Lead1, "MC2_GPU_PARTICLES_LOG", Bool, false, "code", "", "", passthrough | verified),
L(Lead0, "MC2_GPU_PICK_HOVER", Bool, false, "code", "", "", verified),
L(Lead0, "MC2_GPU_PICK_HOVER_TRACE", Bool, false, "code", "", "", verified),
P("MC2_GPU_PROPS_DEBUG_MODE", Int, 0, "props", "", ""),  // default not derivable from the tree
L(Lead1, "MC2_GPU_TRAIL_DISABLE", Bool, false, "code", "", "", verified),
L(Lead0P, "MC2_GROUND_CONTACT_BLOB", Bool, false, "code", "", "", passthrough | verified),
// GvLightsOutClear (VEHICLE-NIGHT-LIT-2)
L(Presence, "MC2_HAZE_PARITY", Bool, false, "terrain", "", "Step 7 (VPL retirement): MC2_HAZE_PARITY probe - env-gated default-off.", verified),
L(Lead0Kill, "MC2_HDRI_BC6H", Bool, true, "gameos", "", "", verified),
L(Lead0Kill, "MC2_HDRI_SKY", Bool, true, "post", "", "", verified),
P("MC2_HDRI_SKY_AZ_OFFSET", Float, 0.0, "post", "", "", verified),
L(Presence, "MC2_HEARTBEAT", Bool, false, "gameos", "", "", passthrough | verified),
P("MC2_HEIGHT", Int, 0, "gameos", "", "", passthrough | verified),
P("MC2_HITCH_MS", Float, 0.0, "gameos", "", "", passthrough | verified),  // default not derivable from the tree
L(Presence, "MC2_HITCH_TRACE", Bool, false, "gameos", "", "", passthrough | verified),
L(Presence, "MC2_HOTKEY_TRACE", Bool, false, "code", "", "Env-gated lifecycle trace for the rebind-to-occupied-key crash.", verified),
// history: docs/gate-history.md#MC2_HUD_IMGUI_TABS
L(Lead0, "MC2_HUD_IMGUI_TABS", Bool, false, "code", "", "IN-MISSION-HUD-IMGUI-SCOPE-1 slice C: data-driven command-bar tab registry (code/hudtabregistry.{h,cpp} + wiring in code/controlgui.cpp).", registry(Feature, "MC2_FEATURE_HUD_IMGUI_TABS", -1, false) | verified),
L(Lead0, "MC2_HUD_IMGUI_TACMAP", Bool, false, "code", "", "", verified),  // default not derivable from the tree
// history: docs/gate-history.md#MC2_HUD_READABILITY_V1
L(Lead0, "MC2_HUD_READABILITY_V1", Bool, false, "unclassified", "", "IN-MISSION-HUD-IMGUI-SCOPE-1 slice A: data-first HUD readability.", registry(Feature, "MC2_FEATURE_HUD_READABILITY_V1", -1, false) | verified),
P("MC2_HUD_SCALE", Float, 0.0, "gameos", "", "", verified),  // default not derivable from the tree
L(Lead0, "MC2_HUD_SIM_SPEED", Bool, false, "code", "", "SIM-SPEED-BUTTON-1: double-gated sim-speed tab (MC2_HUD_SIM_SPEED; needs MC2_HUD_IMGUI_TABS like every registry consumer).", verified),  // default not derivable from the tree
// history: docs/gate-history.md#MC2_HUD_TABS_TEST
L(Lead0, "MC2_HUD_TABS_TEST", Bool, false, "code", "", "IN-MISSION-HUD-IMGUI-SCOPE-1 slice C witness: register a 4th 'test' tab (no legacy button; ImGui chrome via HudImGui::hudShrinkPoint + hudToDisplay, trivial...", registry(Feature, "MC2_FEATURE_HUD_TABS_TEST", -1, false) | verified),
// history: docs/gate-history.md#MC2_HZB_BUILD
G("MC2_HZB_BUILD", Bool, true, "post", "", "Render-only static-prop culling from same-frame Vulkan depth; exact ceil-ladder MIN footprints and per-object LOD. MC2_HZB_BUILD=0 keeps every instance.", passthrough | registry(Feature, "MC2_FEATURE_HZB_BUILD", 18, true) | verified),
L(Presence, "MC2_HZB_FORCE_HORIZON", Bool, false, "code", "", "", verified),
P("MC2_HZB_FORCE_HORIZON_ALT", Float, 150.0, "code", "", "", verified),
P("MC2_HZB_FORCE_HORIZON_ANGLE", Float, 12.0, "code", "", "", verified),
L(Presence, "MC2_HZB_FORCE_HORIZON_POS", Bool, false, "code", "", "Position override is opt-in (MC2_HZB_FORCE_HORIZON_POS=1): place the camera at a map corner.", verified),
P("MC2_HZB_FORCE_HORIZON_ROT", Float, 135.0, "code", "", "", verified),
P("MC2_HZB_FORCE_HORIZON_X", Float, 0.9, "code", "", "Corner position as a fraction of the half-map extent (map is centered at origin: vegHalfMap = worldUnitsMapSide*0.5).", verified),
P("MC2_HZB_FORCE_HORIZON_Y", Float, -0.9, "code", "", "", verified),
G("MC2_HZB_VIEW_FILE", String, "", "code", "", "HZB validation knob (debug only): force a low across-map HORIZON view so static props occlude each other.", verified),  // default not derivable from the tree
L(Lead1, "MC2_IBL_ASSET_STRICT", Bool, false, "unclassified", "", ""),
// history: docs/gate-history.md#MC2_ICON_FAILLOUD
L(Lead0, "MC2_ICON_FAILLOUD", Bool, false, "mech", "", "META-FIX design B PART 3 (code/mechicon.cpp iconTileChecked at the three CPU icon-tile blits): default OFF. =1 substitutes a solid MAGENTA tile when an icon...", passthrough | registry(Feature, "MC2_FEATURE_ICON_FAILLOUD", -1, false) | verified),
// history: docs/gate-history.md#MC2_IFACE_COST_SPLIT
L(Presence, "MC2_IFACE_COST_SPLIT", Bool, false, "code", "", "MISSION-INTERFACE-PERF-1: chrono cost-split of MissionInterfaceManager::update() (the GameLogic.Mission.Interface Tracy zone) incl.", passthrough | registry(Trace, "MC2_TRACE_IFACE_COST_SPLIT", -1, false) | verified),
// history: docs/gate-history.md#MC2_IMGUI
L(Lead0Kill, "MC2_IMGUI", Bool, true, "code", "", "ImGui overlay (GuiRuntime/GuiRuntime.cpp).", registry(Feature, "MC2_FEATURE_IMGUI", 6, true) | verified),
L(Lead1, "MC2_IMGUI_DEMO", Bool, false, "code", "", "", verified),
// history: docs/gate-history.md#MC2_IMGUI_INSPECTOR
L(Lead0Kill, "MC2_IMGUI_INSPECTOR", Bool, true, "code", "", "ImGui inspector panel (GuiRuntime/EditorInspector.cpp).", registry(Feature, "MC2_FEATURE_IMGUI_INSPECTOR", 7, true) | verified),
G("MC2_IMPORTER_DIR", String, "", "unclassified", "", ""),
L(Presence, "MC2_INVIEW_CONFLATION_TRACE", Bool, false, "code", "", "alpha-Stage 1 ?5 Stage 0: candidate-predicate disagreement probe.", passthrough | verified),
L(Presence, "MC2_JUMPJETS_ALL", Bool, false, "code", "", "", verified),
L(Presence, "MC2_LAUNCHED", Bool, false, "gameos", "", "", verified),
L(Lead0Kill, "MC2_LAUNCHER_ENV_JSON", Bool, true, "gameos", "", "", verified),
L(Lead0P, "MC2_LEGACY_INSTANCE_POOLS", Bool, false, "props", "", "", verified),
// LegacyLitMc2Frame (VEHICLE-NIGHT-LIT-1)
// history: docs/gate-history.md#MC2_LEGACY_LIT_MC2_FRAME
L(Lead0Kill, "MC2_LEGACY_LIT_MC2_FRAME", Bool, true, "code", "", "[DEFAULT-ON as of VEHICLE-LIGHTING-PATH-1; MC2_LEGACY_LIT_MC2_FRAME=0 reverts.", passthrough | registry(Feature, "MC2_LEGACY_LIT_MC2_FRAME", 24, true)),
L(Presence, "MC2_LEGACY_LOAD_BANNER", Bool, false, "code", "", "", verified),
// LetterboxOpaque (DETERMINISM-RATCHET-2)
// history: docs/gate-history.md#MC2_LETTERBOX_OPAQUE
L(Lead0Kill, "MC2_LETTERBOX_OPAQUE", Bool, true, "code", "", "Cinematic letterbox bars are drawn OPAQUE black; only their EXTENT animates.", passthrough | registry(Feature, "MC2_FEATURE_LETTERBOX_OPAQUE", -1, true) | verified),
L(Lead0Kill, "MC2_LIGHTBAKE", Bool, true, "code", "", "", verified),
// LIGHTBAKE-REBAKE-ON-READY-1: one-shot static-light re-bake after mission ready.
// history: docs/gate-history.md#MC2_LIGHTBAKE_REBAKE_ON_READY
L(Lead0Kill, "MC2_LIGHTBAKE_REBAKE_ON_READY", Bool, true, "props", "", "LIGHTBAKE-REBAKE-ON-READY-1: soft-invalidate the per-recipe static-light bake cache once, 8 frames after mission-ready, so recipes baked during boot/load...", passthrough | registry(Feature, "MC2_FEATURE_LIGHTBAKE_REBAKE_ON_READY", -1, true) | verified),
// STATICPROP-PERMANENT-INSTANCE-LIGHTS-1 (Slice 1)
L(Lead0Kill, "MC2_LIGHTBRIDGE", Bool, true, "code", "", "", verified),
G("MC2_LIGHTBRIDGE_STABLE_SKIP", Bool, false, "props", "", "Std Bool row: any text other than 0/false/off/no/1/true/on/yes (e.g. =2) still reads as on but is reported badvalue, which MC2_CONFIG_STRICT=1 turns fatal.", verified | passthrough),
G("MC2_LIGHTBRIDGE_STABLE_SKIP_DIAG", Bool, false, "props", "", "Std Bool row: any text other than 0/false/off/no/1/true/on/yes (e.g. =2) still reads as on but is reported badvalue, which MC2_CONFIG_STRICT=1 turns fatal.", verified),
// PERF-HARNESS-TRUTH-1: shipped unregistered, undocumented AND absent from
// the run_smoke.py passthrough allowlist, so it silently no-opped through
// the standard harness -- a lighting investigation could set it, see the
// ordinary lit render, and conclude the debug view showed nothing wrong.
// not Trace: it REPLACES the lit render output
G("MC2_LIGHTING_SHADOW_PREDICATE_MODE", Enum, "", "code", "", "", tokens("|Legacy|Modern") | verified),
L(Presence, "MC2_LIGHTSLOT_TRACE", Bool, false, "props", "", "", verified),
G("MC2_LIGHT_BILLBOARD_FACING", String, "", "props", "", "", passthrough | verified),
L(Lead0, "MC2_LIGHT_BILLBOARD_TWOSIDED", Bool, false, "code", "", "", passthrough | verified),
L(Exact1, "MC2_LIGHT_CLAMP_FIXTURE", Bool, false, "code", "", "MC2_LIGHT_CLAMP_FIXTURE (default OFF) - synthetic injector of deterministic extra point lights into slots [populated..32) to prove slots >16 ever render.", verified),
L(Exact1, "MC2_LIGHT_CLAMP_PROBE", Bool, false, "code", "", "MC2_LIGHT_CLAMP_PROBE (default OFF) - pure observation.", verified),
L(Lead1, "MC2_LIGHT_CONE_AIM_FLIP", Bool, false, "code", "", "", passthrough | verified),
L(Lead1, "MC2_LIGHT_CONE_DIAG", Bool, false, "props", "", "", passthrough | verified),
L(Lead0Kill, "MC2_LIGHT_CONE_GLOW", Bool, true, "code", "", "", passthrough | verified),
P("MC2_LIGHT_CONE_GLOW_ALPHA", Float, 40.0, "code", "", "", passthrough | clampRange(0.0, 255.0) | verified),
// history: docs/gate-history.md#MC2_LIGHT_CONE_NODES
L(Lead0Kill, "MC2_LIGHT_CONE_NODES", Bool, true, "code", "", "LIGHT-CONE-MODELS-1: DEFAULT-ON.", passthrough | registry(Trace, "MC2_LIGHT_CONE_NODES", -1, true) | verified),
P("MC2_LIGHT_CONE_SPILL", Float, 1.0, "code", "", "", passthrough | clampRange(0.0, 3.0) | verified),
L(Lead1, "MC2_LIGHT_CONE_VIS_GATE", Bool, false, "code", "", "", passthrough | verified),
L(Presence, "MC2_LIGHT_COST_SPLIT", Bool, false, "terrain", "", "", passthrough | verified),
L(Lead0Kill, "MC2_LIGHT_CULL_RADIUS", Bool, true, "code", "", "", passthrough | verified),
// LIGHT-CLUSTER-PERF-1: recover the frame time LIGHT-POOL-CAP-1 spent when it
// raised the pool cap 64 -> 256 (the fix was correct -- mc2_10 wants 142
// fixtures -- but the consumer loop is per-fragment, so cost scaled with the
// whole pool).
// history: docs/gate-history.md#MC2_LIGHT_GRID
L(Lead0Kill, "MC2_LIGHT_GRID", Bool, true, "terrain", "", "LIGHT-CLUSTER-PERF-1: world-space XY light GRID over the per-frame terrain light pool.", passthrough | verified | registry(Feature, "MC2_FEATURE_LIGHT_GRID", -1, true)),
// history: docs/gate-history.md#MC2_LIGHT_GRID_DIM
P("MC2_LIGHT_GRID_DIM", Int, 64, "terrain", "", "LIGHT-CLUSTER-PERF-1 knob: cells per axis for the light grid (grid is DIM x DIM over the XY bbox of this frame's packed lights).", passthrough | range(2, 256) | verified | registry(Trace, "MC2_TUNE_LIGHT_GRID_DIM", -1, false)),
L(Lead1, "MC2_LIGHT_MOVER_VIS_GATE", Bool, false, "code", "", "", passthrough | verified),
// NIGHT-LIGHT-POOLS-3: overlapping pool sums are tone-compressed instead of
// hard-clamped (5-mech group pools no longer flat-top a saturated blob).
// history: docs/gate-history.md#MC2_LIGHT_POOL_COMPRESS
L(Lead0Kill, "MC2_LIGHT_POOL_COMPRESS", Bool, true, "terrain", "", "NIGHT-LIGHT-POOLS-3: tone-compress the accumulated terrain light-pool sum (chunk + overlay frag): 1-exp(-1.6*sum) instead of min(sum,1).", passthrough | verified | registry(Feature, "MC2_FEATURE_LIGHT_POOL_COMPRESS", -1, true)),
L(Lead1, "MC2_LIGHT_POOL_DUMP", Bool, false, "terrain", "", "", passthrough | verified),
// LIGHT-POOL-CAP-1 (5213d257) shipped this override unregistered and
// undocumented -- registered here by LIGHT-CLUSTER-PERF-1's meta review.
// history: docs/gate-history.md#MC2_LIGHT_POOL_MAX
P("MC2_LIGHT_POOL_MAX", Int, 0, "terrain", "", "LIGHT-POOL-CAP-1: per-mission slot count of the terrain light pool (gos_terrain_lighting.cpp mission_init).", passthrough | range(0, 4096) | verified | registry(Trace, "MC2_TUNE_LIGHT_POOL_MAX", -1, false)),
G("MC2_LIGHT_POOL_MOVER_COLOR", String, "", "code", "", "MOVER-LIGHT-HALOGEN-1: hex RRGGBB mover light colour; unset, empty or malformed keeps FFEED1.", passthrough | verified),
P("MC2_LIGHT_POOL_MOVER_DIM", Float, 0.25, "terrain", "", "", passthrough | range(0.0, 1.0) | verified),
P("MC2_LIGHT_POOL_MOVER_FWD", Float, 24.0, "code", "", "", passthrough | clampRange(0.0, 120.0) | verified),
L(Lead0Kill, "MC2_LIGHT_POOL_MOVER_MAXBLEND", Bool, true, "terrain", "", "", passthrough | verified),
P("MC2_LIGHT_POOL_MOVER_RADIUS", Float, 0.6, "terrain", "", "", passthrough | rangeAbove(0.0, 1.0) | verified),
L(Lead0Kill, "MC2_LIGHT_POOL_OBJECTS", Bool, true, "terrain", "", "", passthrough | verified),
P("MC2_LIGHT_POOL_OBJ_SCALE", Float, 0.7, "terrain", "", "", passthrough | clampRange(0.0, 2.0) | verified),
L(Lead0Kill, "MC2_LIGHT_POOL_SOFT", Bool, true, "terrain", "", "", passthrough | verified),
L(Lead0Kill, "MC2_LIGHT_POOL_VIS_GATE", Bool, true, "terrain", "", "", verified),
// LightPosColumnFix (LIGHT-POS-COLUMN-1)
// history: docs/gate-history.md#MC2_LIGHT_POS_COLUMN_FIX
L(Lead0Kill, "MC2_LIGHT_POS_COLUMN_FIX", Bool, true, "code", "", "[DEFAULT-ON as of VEHICLE-LIGHTING-PATH-1; MC2_LIGHT_POS_COLUMN_FIX=0 reverts.", passthrough | registry(Feature, "MC2_LIGHT_POS_COLUMN_FIX", 25, true) | verified),
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
L(Exact1, "MC2_LIGHT_ROW_CENSUS", Bool, false, "code", "", "MC2_LIGHT_ROW_CENSUS (default OFF) - LIGHT-POS-COMBAT-1.", verified | passthrough),
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
L(Presence, "MC2_LINKUP_TRACE", Bool, false, "mech", "", "ISSUE-50 diagnostics (code/mech.cpp BattleMech::update): for every off-GUI mech, periodically (~1/s) logs [LINKUP] part/name/status/los, plus [LINKUP]...", passthrough | registry(Trace, "MC2_TRACE_LINKUP", -1, false) | verified),
// history: docs/gate-history.md#MC2_LOAD_RACE_DELAY_ATLAS
L(Presence, "MC2_LOAD_RACE_DELAY_ATLAS", Bool, false, "terrain", "", "gos_terrain_indirect.cpp negative control for the mission-load residency-race probes: when set (any value), BuildColormapAtlas() defers the real GL atlas...", registry(Trace, "MC2_TRACE_LOAD_RACE_DELAY_ATLAS", -1, false) | verified),
// history: docs/gate-history.md#MC2_LOAD_RACE_PROBE_FRAMES
G("MC2_LOAD_RACE_PROBE_FRAMES", String, "", "gameos", "", "gameosmain.cpp: overrides the comma-separated list of frame numbers at which the mission-load residency-race one-shot diagnostics probe fires (see...", registry(Trace, "MC2_TRACE_LOAD_RACE_PROBE_FRAMES", -1, false) | verified),
L(Presence, "MC2_LOAD_RACE_TRACE", Bool, false, "code", "", "MC2_LOAD_RACE_TRACE: gated, one-shot mission-load residency-race diagnostics.", verified),
P("MC2_LOAD_RACE_VERDICT_FRAME", Int, 0, "gameos", "", "", verified),  // default not derivable from the tree
// MC2_LOG master log gate
// history: docs/gate-history.md#MC2_LOG
G("MC2_LOG", String, "", "gameos", "", "Master stdout log gate (gameosmain.cpp, right after setvbuf).", passthrough | registry(Trace, "MC2_LOG", -1, false) | verified),
L(Presence, "MC2_LOG_CURSOR", Bool, false, "code", "", "---- MC2_LOG_CURSOR diagnostics (instrumentation only) ---- Logistics::start does NOT call mouseOn() (it is commented out) -- it relies on drawMouse already...", verified),
G("MC2_LOG_DECORATE", String, "", "gameos", "", "", verified),  // default not derivable from the tree
// history: docs/gate-history.md#MC2_LOG_FILE_RESOLVE
G("MC2_LOG_FILE_RESOLVE", String, "", "code", "", "META-FIX design E tier 3 (mclib/file.cpp, parsed at InitModSearchPaths): file-resolution ladder diagnostics.", passthrough | registry(Trace, "MC2_TRACE_LOG_FILE_RESOLVE", -1, false) | verified),
L(Presence, "MC2_LOG_LOGISTICS", Bool, false, "mech", "", "", passthrough | verified),
